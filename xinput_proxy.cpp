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
#include <cwchar>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace winrt::Windows::Gaming::Input;

// ---- Settings (loaded from fh4_trigger.ini next to the DLL; reloads live) ----
struct Config {
    bool   log = true;
    bool   enabled = true;
    bool   disableScreenReader = true; // report 'no screen reader running' to the game
    int    port = 5300;               // restart needed to change
    // Telemetry mode
    double slipStart = 0.35;          // slip ratio where the effect begins
    double slipFull = 1.5;            // slip ratio for full strength
    double maxStrength = 0.9;         // cap, 0..1
    double pulseHz = 14.0;            // buzz rate
    double pulseLow = 0.35;           // strength of the "off" half of each pulse, 0..1
    double minSpeed = 2.0;            // m/s, ignore effects when nearly stopped
    double leftGain = 1.0;            // brake trigger multiplier
    double rightGain = 1.0;           // throttle trigger multiplier
    // Input-only mode (used when no telemetry arrives)
    bool   inputEnabled = true;
    double humBase = 0.08;
    double humScale = 0.20;
    double burstStrength = 0.7;
    double burstThreshold = 0.25;     // throttle jump (0..1) that triggers the kick
    int    burstMs = 150;
    double chatterThreshold = 0.85;   // brake pull (0..1) where chatter starts
    double chatterStrength = 0.55;
    double lightBrake = 0.10;
};
static Config g_cfg;
static wchar_t g_iniPath[MAX_PATH] = L"fh4_trigger.ini";
static FILETIME g_iniTime = {};

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
    if (!g_cfg.log) return;
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
static volatile LONG      g_throttle = 0;       // latest right trigger 0..255 seen via XInputGetState
static volatile LONG      g_brake = 0;          // latest left trigger 0..255
static volatile ULONGLONG g_lastTelemetry = 0;  // tick of last telemetry packet

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
    if (a <= g_cfg.slipStart) return 0.0;
    double span = std::max(0.001, g_cfg.slipFull - g_cfg.slipStart);
    return std::min(1.0, (a - g_cfg.slipStart) / span);
}

static double Pulse() {
    double t = (double)GetTickCount64() / 1000.0;
    return std::fmod(t * g_cfg.pulseHz, 1.0) < 0.5 ? 1.0 : g_cfg.pulseLow;
}

// ---- Config file ----
static const char* DEFAULT_INI =
"; FH4 trigger rumble settings. Changes apply within about a second while the game runs\r\n"
"; (except udp_port, which needs a restart). Values are 0..1 unless noted.\r\n"
"\r\n"
"[General]\r\n"
"enabled=1            ; 0 turns all trigger effects off\r\n"
"log=1                ; 0 stops writing fh4_trigger_proxy.log\r\n"
"udp_port=5300        ; must match FH4 Data Out port\r\n"
"\r\n"
"; Wheelspin / lock-up effects (needs FH4 Data Out on)\r\n"
"[Telemetry]\r\n"
"slip_start=0.35      ; slip ratio where the effect begins (lower = more sensitive)\r\n"
"slip_full=1.5        ; slip ratio for full strength\r\n"
"max_strength=0.9     ; overall cap\r\n"
"left_gain=1.0        ; brake trigger multiplier\r\n"
"right_gain=1.0       ; throttle trigger multiplier\r\n"
"pulse_hz=14          ; buzz rate\r\n"
"pulse_low=0.35       ; strength during the quiet half of each pulse (1 = steady, 0 = on/off)\r\n"
"min_speed=2.0        ; m/s, no effects below this speed unless braking\r\n"
"\r\n"
"; Used automatically when no telemetry arrives\r\n"
"[InputOnly]\r\n"
"enabled=1\r\n"
"hum_base=0.08        ; throttle hum at light pull\r\n"
"hum_scale=0.20       ; extra hum at full pull\r\n"
"burst_strength=0.7   ; kick when you stab the throttle\r\n"
"burst_threshold=0.25 ; how sudden the stab must be\r\n"
"burst_ms=150         ; kick length in milliseconds\r\n"
"chatter_threshold=0.85 ; brake pull where chatter starts\r\n"
"chatter_strength=0.55\r\n"
"light_brake=0.10     ; faint buzz at lighter braking\r\n"
"\r\n"
"; Tells the game no screen reader is running (stops menu narration if the game asks Windows)\r\n"
"[Accessibility]\r\n"
"disable_screen_reader=1\r\n";

static double CfgD(const wchar_t* sec, const wchar_t* key, double def, double lo, double hi) {
    wchar_t dflt[64], buf[64];
    swprintf_s(dflt, L"%g", def);
    GetPrivateProfileStringW(sec, key, dflt, buf, 64, g_iniPath);
    double v = _wtof(buf);             // text after the number (comments) is ignored
    if (!(v == v)) v = def;
    return std::min(hi, std::max(lo, v));
}
static bool CfgB(const wchar_t* sec, const wchar_t* key, bool def) {
    return CfgD(sec, key, def ? 1 : 0, 0, 1) >= 0.5;
}

static void LoadConfig(bool first) {
    g_cfg.enabled        = CfgB(L"General", L"enabled", true);
    g_cfg.log            = CfgB(L"General", L"log", true);
    g_cfg.disableScreenReader = CfgB(L"Accessibility", L"disable_screen_reader", true);
    if (first) g_cfg.port = (int)CfgD(L"General", L"udp_port", 5300, 1, 65535);

    g_cfg.slipStart      = CfgD(L"Telemetry", L"slip_start", 0.35, 0, 10);
    g_cfg.slipFull       = CfgD(L"Telemetry", L"slip_full", 1.5, 0.01, 20);
    g_cfg.maxStrength    = CfgD(L"Telemetry", L"max_strength", 0.9, 0, 1);
    g_cfg.leftGain       = CfgD(L"Telemetry", L"left_gain", 1.0, 0, 5);
    g_cfg.rightGain      = CfgD(L"Telemetry", L"right_gain", 1.0, 0, 5);
    g_cfg.pulseHz        = CfgD(L"Telemetry", L"pulse_hz", 14, 0.5, 60);
    g_cfg.pulseLow       = CfgD(L"Telemetry", L"pulse_low", 0.35, 0, 1);
    g_cfg.minSpeed       = CfgD(L"Telemetry", L"min_speed", 2.0, 0, 100);

    g_cfg.inputEnabled   = CfgB(L"InputOnly", L"enabled", true);
    g_cfg.humBase        = CfgD(L"InputOnly", L"hum_base", 0.08, 0, 1);
    g_cfg.humScale       = CfgD(L"InputOnly", L"hum_scale", 0.20, 0, 1);
    g_cfg.burstStrength  = CfgD(L"InputOnly", L"burst_strength", 0.7, 0, 1);
    g_cfg.burstThreshold = CfgD(L"InputOnly", L"burst_threshold", 0.25, 0.01, 1);
    g_cfg.burstMs        = (int)CfgD(L"InputOnly", L"burst_ms", 150, 10, 2000);
    g_cfg.chatterThreshold = CfgD(L"InputOnly", L"chatter_threshold", 0.85, 0, 1);
    g_cfg.chatterStrength  = CfgD(L"InputOnly", L"chatter_strength", 0.55, 0, 1);
    g_cfg.lightBrake     = CfgD(L"InputOnly", L"light_brake", 0.10, 0, 1);
    Log("config loaded (enabled=%d, port=%d)", g_cfg.enabled ? 1 : 0, g_cfg.port);
}

static void EnsureIni() {
    HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;          // already exists, or folder not writable
    DWORD w; WriteFile(h, DEFAULT_INI, (DWORD)strlen(DEFAULT_INI), &w, nullptr);
    CloseHandle(h);
    Log("wrote default fh4_trigger.ini");
}

static bool IniChanged() {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &d)) return false;
    if (CompareFileTime(&d.ftLastWriteTime, &g_iniTime) == 0) return false;
    g_iniTime = d.ftLastWriteTime;
    return true;
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
        v.LeftTrigger  = std::min(g_cfg.maxStrength, left * g_cfg.leftGain);
        v.RightTrigger = std::min(g_cfg.maxStrength, right * g_cfg.rightGain);
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

    EnsureIni();
    IniChanged();            // remember current file time
    LoadConfig(true);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return 0; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { Log("socket failed"); return 0; }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("bind to 127.0.0.1:%d failed (error %d)", g_cfg.port, WSAGetLastError());
        return 0;
    }
    DWORD timeoutMs = 16;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    Log("telemetry listening on 127.0.0.1:%d", g_cfg.port);

    bool loggedFirst = false;
    char buf[1024];
    double prevT = 0.0;
    ULONGLONG burstUntil = 0;
    bool loggedInput = false;
    ULONGLONG nextCfgCheck = 0;
    for (;;) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (GetTickCount64() >= nextCfgCheck) {
            nextCfgCheck = GetTickCount64() + 1000;
            if (IniChanged()) LoadConfig(false);
        }
        double left = 0.0, right = 0.0;

        if (n >= MIN_LEN) {
            g_lastTelemetry = GetTickCount64();
            if (!loggedFirst) { Log("first telemetry packet, %d bytes", n); loggedFirst = true; }
            if (Rd<int>(buf, OFF_RACE_ON) != 0) {
                float fl = Rd<float>(buf, OFF_SLIP), fr = Rd<float>(buf, OFF_SLIP + 4);
                float rl = Rd<float>(buf, OFF_SLIP + 8), rr = Rd<float>(buf, OFF_SLIP + 12);
                int   drive = Rd<int>(buf, OFF_DRIVETRAIN);
                float speed = Rd<float>(buf, OFF_SPEED);
                double accel = (unsigned char)buf[OFF_ACCEL] / 255.0;
                double brake = (unsigned char)buf[OFF_BRAKE] / 255.0;

                if (speed >= g_cfg.minSpeed || brake >= 0.1) {
                    double driven;
                    if (drive == 0)      driven = std::max(Scale(fl), Scale(fr));
                    else if (drive == 1) driven = std::max(Scale(rl), Scale(rr));
                    else                 driven = std::max(std::max(Scale(fl), Scale(fr)), std::max(Scale(rl), Scale(rr)));
                    double lock = std::max(std::max(Scale(fl), Scale(fr)), std::max(Scale(rl), Scale(rr)));
                    if (accel > 0.2) right = driven * accel * Pulse();
                    if (brake > 0.2) left  = lock * brake * Pulse();
                }
            }
        } else if (g_cfg.inputEnabled && GetTickCount64() - g_lastTelemetry > 1000) {
            // No telemetry: generic effect from trigger pull only
            if (!loggedInput) { Log("no telemetry, using input-only mode"); loggedInput = true; }
            double t = g_throttle / 255.0, b = g_brake / 255.0;
            ULONGLONG now = GetTickCount64();
            if (t - prevT > g_cfg.burstThreshold) burstUntil = now + (ULONGLONG)g_cfg.burstMs;
            prevT = t;
            if (now < burstUntil)  right = g_cfg.burstStrength;
            else if (t > 0.1)      right = g_cfg.humBase + g_cfg.humScale * t;
            if (b > g_cfg.chatterThreshold) left = g_cfg.chatterStrength * Pulse();
            else if (b > 0.1)      left = g_cfg.lightBrake * b;
        }
        if (!g_cfg.enabled) { left = 0.0; right = 0.0; }
        ApplyTriggers(left, right);
    }
    return 0;
}

static void InstallScreenReaderPatch(const char* phase);   // defined below

static BOOL CALLBACK StartThread(PINIT_ONCE, PVOID, PVOID*) {
    InstallScreenReaderPatch("first XInput call");
    HANDLE h = CreateThread(nullptr, 0, TelemetryThread, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); Log("telemetry thread started"); }
    else   { Log("CreateThread failed"); }
    return TRUE;
}

// ---- Screen reader patch (in-memory only; no game files are modified) ----
// Many games ask Windows "is a screen reader running?" via SystemParametersInfo(SPI_GETSCREENREADER)
// and enable menu narration if the answer is yes. We patch the game's import of that function
// so the answer is "no" when disable_screen_reader=1.
typedef BOOL(WINAPI* SPIW_t)(UINT, UINT, PVOID, UINT);
typedef BOOL(WINAPI* SPIA_t)(UINT, UINT, PVOID, UINT);
static SPIW_t real_SPIW = nullptr;
static SPIA_t real_SPIA = nullptr;

static void ScreenReaderFixup(UINT action, PVOID out, BOOL ok, const char* which) {
    if (action != SPI_GETSCREENREADER || !out || !ok) return;
    static LONG logged = 0;
    if (InterlockedExchange(&logged, 1) == 0)
        Log("game queried SPI_GETSCREENREADER via %s (Windows said %d, override %s)",
            which, *(BOOL*)out ? 1 : 0, g_cfg.disableScreenReader ? "on" : "off");
    if (g_cfg.disableScreenReader) *(BOOL*)out = FALSE;
}
static BOOL WINAPI Hook_SPIW(UINT a, UINT b, PVOID c, UINT d) {
    BOOL r = real_SPIW(a, b, c, d);
    ScreenReaderFixup(a, c, r, "SystemParametersInfoW");
    return r;
}
static BOOL WINAPI Hook_SPIA(UINT a, UINT b, PVOID c, UINT d) {
    BOOL r = real_SPIA(a, b, c, d);
    ScreenReaderFixup(a, c, r, "SystemParametersInfoA");
    return r;
}

// Patches the exe's import slot only if it already holds the real function address
// (i.e. the loader has finished filling it in). Returns true when hooked.
static bool PatchImport(HMODULE mod, const char* dll, const char* func, void* expected, void* hook) {
    BYTE* base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !expected) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto look  = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; look->u1.AddressOfData; ++look, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(look->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + look->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            if ((void*)thunk->u1.Function != expected) return false;   // not resolved yet (or already hooked)
            DWORD old;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

static void InstallScreenReaderPatch(const char* phase) {
    static bool doneW = false, doneA = false;
    if (doneW && doneA) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!exe || !u32) { Log("screen reader patch (%s): user32 not loaded yet", phase); return; }
    if (!doneW) {
        void* addr = (void*)GetProcAddress(u32, "SystemParametersInfoW");
        if (PatchImport(exe, "user32.dll", "SystemParametersInfoW", addr, (void*)Hook_SPIW)) {
            real_SPIW = (SPIW_t)addr; doneW = true;
            Log("screen reader patch (%s): SystemParametersInfoW hooked", phase);
        }
    }
    if (!doneA) {
        void* addr = (void*)GetProcAddress(u32, "SystemParametersInfoA");
        if (PatchImport(exe, "user32.dll", "SystemParametersInfoA", addr, (void*)Hook_SPIA)) {
            real_SPIA = (SPIA_t)addr; doneA = true;
            Log("screen reader patch (%s): SystemParametersInfoA hooked", phase);
        }
    }
    if (!doneW && !doneA) Log("screen reader patch (%s): nothing hooked yet", phase);
}

static DWORD Capture(DWORD r, DWORD i, XINPUT_STATE* s) {
    if (r == ERROR_SUCCESS && s && i == 0) {
        g_throttle = s->Gamepad.bRightTrigger;
        g_brake = s->Gamepad.bLeftTrigger;
    }
    return r;
}

// ---- Exported (proxy) functions ----
extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
    InitOnceExecuteOnce(&g_threadOnce, StartThread, nullptr, nullptr); // not in DllMain: loader lock
    return Capture(r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED, i, s);
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
    InitOnceExecuteOnce(&g_threadOnce, StartThread, nullptr, nullptr);
    if (r_GetStateEx) return Capture(r_GetStateEx(i, s), i, s);
    return Capture(r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED, i, s);
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
                wcscpy_s(g_iniPath, path);
                wchar_t* dot = wcsrchr(g_iniPath, L'\\');
                if (dot) { *(dot + 1) = 0; wcscat_s(g_iniPath, L"fh4_trigger.ini"); }
            }
        }
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char exeA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeA, MAX_PATH, nullptr, nullptr);
        Log("=== BUILD v9 (early screen reader patch) === attached to: %s", exeA);
        InstallScreenReaderPatch("DLL load");
    }
    return TRUE;
}
