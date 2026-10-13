// FH4 trigger rumble - in-process XInput proxy (v5)
//
// The game only uses XInput for READING the controller; its rumble goes
// elsewhere, so we cannot intercept it. Instead this DLL (running inside the
// game, which has window focus) listens to FH4's "Data Out" UDP telemetry on
// 127.0.0.1:5300 and drives the impulse triggers through Windows.Gaming.Input:
//   right trigger: wheelspin under throttle
//   left trigger : wheel lock-up / ABS under braking
//
// All XInput calls are forwarded unchanged to the real system xinput1_4.dll.
// Log: fh4_trigger_proxy.log next to this DLL.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <Xinput.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace winrt::Windows::Gaming::Input;

// ---- Tuning ----
static const bool   ENABLE_LOG    = true;
static const int    UDP_PORT      = 5300;
static const double SLIP_START    = 0.35;  // slip ratio where the effect begins
static const double SLIP_FULL     = 1.5;   // slip ratio for full strength
static const double MAX_STRENGTH  = 0.9;   // cap, 0..1
static const double PULSE_HZ      = 14.0;  // buzz rate
static const double MIN_SPEED     = 2.0;   // m/s, ignore effects when nearly stopped

// FH4 Data Out layout (bytes)
static const int OFF_RACE_ON   = 0;
static const int OFF_SLIP      = 84;   // 4 floats FL FR RL RR
static const int OFF_DRIVETRAIN= 224;  // 0 FWD, 1 RWD, 2 AWD
static const int OFF_SPEED     = 256;
static const int OFF_ACCEL     = 315;  // u8
static const int OFF_BRAKE     = 316;  // u8
static const int MIN_LEN       = 323;

// ---- Logging ----
static wchar_t g_logPath[MAX_PATH] = L"fh4_trigger_proxy.log";

static void Log(const char* fmt, ...) {
    if (!ENABLE_LOG) return;
    char buf[512];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, a);
    va_end(a);
    if (n < 0) return;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, buf, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

#define LOG_FIRST() do { static LONG c_ = 0; if (InterlockedIncrement(&c_) == 1) Log("first call: %s", __func__); } while (0)

// ---- Real XInput ----
typedef DWORD(WINAPI* GetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* SetState_t)(DWORD, XINPUT_VIBRATION*);
typedef DWORD(WINAPI* GetCaps_t)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void (WINAPI* Enable_t)(BOOL);
typedef DWORD(WINAPI* Battery_t)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD(WINAPI* Keystroke_t)(DWORD, DWORD, PXINPUT_KEYSTROKE);
typedef DWORD(WINAPI* AudioIds_t)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);

static HMODULE g_real = nullptr;
static GetState_t  r_GetState, r_GetStateEx;
static SetState_t  r_SetState;
static GetCaps_t   r_GetCaps;
static Enable_t    r_Enable;
static Battery_t   r_Battery;
static Keystroke_t r_Keystroke;
static AudioIds_t  r_AudioIds;
static INIT_ONCE   g_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE   g_threadOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\xinput1_4.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) { Log("Failed to load real xinput1_4.dll"); return TRUE; }
    r_GetState   = (GetState_t)GetProcAddress(g_real, "XInputGetState");
    r_GetStateEx = (GetState_t)GetProcAddress(g_real, MAKEINTRESOURCEA(100));
    r_SetState   = (SetState_t)GetProcAddress(g_real, "XInputSetState");
    r_GetCaps    = (GetCaps_t)GetProcAddress(g_real, "XInputGetCapabilities");
    r_Enable     = (Enable_t)GetProcAddress(g_real, "XInputEnable");
    r_Battery    = (Battery_t)GetProcAddress(g_real, "XInputGetBatteryInformation");
    r_Keystroke  = (Keystroke_t)GetProcAddress(g_real, "XInputGetKeystroke");
    r_AudioIds   = (AudioIds_t)GetProcAddress(g_real, "XInputGetAudioDeviceIds");
    Log("Proxy loaded, real XInput resolved");
    return TRUE;
}
static void EnsureReal() { InitOnceExecuteOnce(&g_once, InitReal, nullptr, nullptr); }

// ---- Telemetry -> trigger effects ----
template <class T> static T Rd(const char* b, int off) { T v; memcpy(&v, b + off, sizeof(T)); return v; }

static double Scale(float x) {
    double a = std::fabs((double)x);
    if (a <= SLIP_START) return 0.0;
    return std::min(1.0, (a - SLIP_START) / (SLIP_FULL - SLIP_START));
}

static double Pulse() {
    double t = (double)GetTickCount64() / 1000.0;
    return std::fmod(t * PULSE_HZ, 1.0) < 0.5 ? 1.0 : 0.35;
}

static void ApplyTriggers(double left, double right) {
    static Gamepad pad{ nullptr };
    static double lastL = -1, lastR = -1;
    static bool loggedOk = false, loggedFail = false;

    if (left == 0.0 && right == 0.0 && lastL == 0.0 && lastR == 0.0) return; // nothing to change

    try {
        if (!pad) {
            auto pads = Gamepad::Gamepads();
            if (pads.Size() == 0) return;
            pad = pads.GetAt(0);
            Log("gamepad found via Windows.Gaming.Input");
        }
        GamepadVibration v{};
        try { v = pad.Vibration(); } catch (...) {}   // keep any main-motor values
        v.LeftTrigger  = std::min(MAX_STRENGTH, left);
        v.RightTrigger = std::min(MAX_STRENGTH, right);
        pad.Vibration(v);
        lastL = left; lastR = right;
        if (!loggedOk && (left > 0 || right > 0)) { Log("trigger write OK"); loggedOk = true; }
    } catch (...) {
        if (!loggedFail) { Log("trigger write FAILED"); loggedFail = true; }
        pad = nullptr;
    }
}

static DWORD WINAPI TelemetryThread(LPVOID) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return 0; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { Log("socket failed"); return 0; }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("bind to 127.0.0.1:%d failed (error %d)", UDP_PORT, WSAGetLastError());
        return 0;
    }
    DWORD timeoutMs = 60;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    Log("telemetry listening on 127.0.0.1:%d", UDP_PORT);

    bool loggedFirst = false;
    char buf[1024];
    for (;;) {
        int n = recv(sock, buf, sizeof(buf), 0);
        double left = 0.0, right = 0.0;

        if (n >= MIN_LEN) {
            if (!loggedFirst) { Log("first telemetry packet, %d bytes", n); loggedFirst = true; }
            if (Rd<int>(buf, OFF_RACE_ON) != 0) {
                float fl = Rd<float>(buf, OFF_SLIP), fr = Rd<float>(buf, OFF_SLIP + 4);
                float rl = Rd<float>(buf, OFF_SLIP + 8), rr = Rd<float>(buf, OFF_SLIP + 12);
                int   drive = Rd<int>(buf, OFF_DRIVETRAIN);
                float speed = Rd<float>(buf, OFF_SPEED);
                double accel = (unsigned char)buf[OFF_ACCEL] / 255.0;
                double brake = (unsigned char)buf[OFF_BRAKE] / 255.0;

                if (speed >= MIN_SPEED || brake >= 0.1) {
                    double driven;
                    if (drive == 0)      driven = std::max(Scale(fl), Scale(fr));
                    else if (drive == 1) driven = std::max(Scale(rl), Scale(rr));
                    else                 driven = std::max(std::max(Scale(fl), Scale(fr)), std::max(Scale(rl), Scale(rr)));
                    double lock = std::max(std::max(Scale(fl), Scale(fr)), std::max(Scale(rl), Scale(rr)));
                    if (accel > 0.2) right = driven * accel * Pulse();
                    if (brake > 0.2) left  = lock * brake * Pulse();
                }
            }
        }
        ApplyTriggers(left, right);
    }
    return 0;
}

static BOOL CALLBACK StartThread(PINIT_ONCE, PVOID, PVOID*) {
    HANDLE h = CreateThread(nullptr, 0, TelemetryThread, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); Log("telemetry thread started"); }
    else   { Log("CreateThread failed"); }
    return TRUE;
}

// ---- Exported (proxy) functions ----
extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
    InitOnceExecuteOnce(&g_threadOnce, StartThread, nullptr, nullptr); // not in DllMain: loader lock
    return r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
    InitOnceExecuteOnce(&g_threadOnce, StartThread, nullptr, nullptr);
    if (r_GetStateEx) return r_GetStateEx(i, s);
    return r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputSetState(DWORD i, XINPUT_VIBRATION* v) {
    EnsureReal();
    LOG_FIRST();
    return r_SetState ? r_SetState(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES* c) {
    EnsureReal();
    LOG_FIRST();
    return r_GetCaps ? r_GetCaps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI Proxy_XInputEnable(BOOL e) {
    EnsureReal();
    LOG_FIRST();
    if (r_Enable) r_Enable(e);
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD i, BYTE t, XINPUT_BATTERY_INFORMATION* b) {
    EnsureReal();
    LOG_FIRST();
    return r_Battery ? r_Battery(i, t, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD i, DWORD r, PXINPUT_KEYSTROKE k) {
    EnsureReal();
    LOG_FIRST();
    return r_Keystroke ? r_Keystroke(i, r, k) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetAudioDeviceIds(DWORD i, LPWSTR a, UINT* ac, LPWSTR b, UINT* bc) {
    EnsureReal();
    LOG_FIRST();
    return r_AudioIds ? r_AudioIds(i, a, ac, b, bc) : ERROR_DEVICE_NOT_CONNECTED;
}

// Exports that exist in older XInput (1_3 / 9_1_0) but not in 1_4
DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD, GUID*, GUID*) { return ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI Proxy_Ordinal101(DWORD, DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal102(DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal103(DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal104(DWORD, void*) { return ERROR_CALL_NOT_IMPLEMENTED; }

} // extern "C"

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) {
                *(slash + 1) = 0;
                wcscat_s(path, L"fh4_trigger_proxy.log");
                wcscpy_s(g_logPath, path);
            }
        }
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char exeA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeA, MAX_PATH, nullptr, nullptr);
        Log("=== BUILD v5 (in-process telemetry triggers) === attached to: %s", exeA);
    }
    return TRUE;
}
