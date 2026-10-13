// FH4 trigger rumble XInput proxy
// Forwards all XInput calls to the real system xinput1_4.dll. On every
// XInputSetState call it also drives the controller's impulse triggers via
// Windows.Gaming.Input, derived from the motor values the game sent.
//
// Logs calls to fh4_trigger_proxy.log next to the game exe so you can confirm
// the game actually uses this DLL.

#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <cstdarg>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <cstdio>
#include <algorithm>

using namespace winrt::Windows::Gaming::Input;

// ---- Tuning ----
static const double LEFT_TRIGGER_GAIN  = 0.9;  // from left (heavy) motor  -> brake trigger
static const double RIGHT_TRIGGER_GAIN = 0.9;  // from right (light) motor -> throttle trigger
static const bool   PASS_MAIN_MOTORS   = true; // keep main motors at the game's values
static const bool   ENABLE_LOG         = true;

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
static bool        g_winrtOk = false;

static void Log(const char* fmt, ...) {
    if (!ENABLE_LOG) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "fh4_trigger_proxy.log", "a") != 0 || !f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

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

static void DriveTriggers(DWORD index, const XINPUT_VIBRATION& v) {
    try {
        if (!g_winrtOk) {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
            g_winrtOk = true;
        }
        auto pads = Gamepad::Gamepads();
        if (index >= pads.Size()) return;

        const double l = v.wLeftMotorSpeed  / 65535.0;
        const double r = v.wRightMotorSpeed / 65535.0;

        GamepadVibration gv{};
        gv.LeftMotor    = PASS_MAIN_MOTORS ? l : 0.0;
        gv.RightMotor   = PASS_MAIN_MOTORS ? r : 0.0;
        gv.LeftTrigger  = std::min(1.0, l * LEFT_TRIGGER_GAIN);
        gv.RightTrigger = std::min(1.0, r * RIGHT_TRIGGER_GAIN);
        pads.GetAt(index).Vibration(gv);
    } catch (...) {
        static bool logged = false;
        if (!logged) { Log("WinRT trigger write failed"); logged = true; }
    }
}

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    return r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    if (r_GetStateEx) return r_GetStateEx(i, s);
    return r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputSetState(DWORD i, XINPUT_VIBRATION* v) {
    EnsureReal();
    DWORD res = r_SetState ? r_SetState(i, v) : ERROR_DEVICE_NOT_CONNECTED;
    if (v) {
        static int n = 0;
        if (n++ < 5) Log("XInputSetState idx=%lu L=%u R=%u", i, v->wLeftMotorSpeed, v->wRightMotorSpeed);
        DriveTriggers(i, *v);
    }
    return res;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES* c) {
    EnsureReal();
    return r_GetCaps ? r_GetCaps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI Proxy_XInputEnable(BOOL e) {
    EnsureReal();
    if (r_Enable) r_Enable(e);
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD i, BYTE t, XINPUT_BATTERY_INFORMATION* b) {
    EnsureReal();
    return r_Battery ? r_Battery(i, t, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD i, DWORD r, PXINPUT_KEYSTROKE k) {
    EnsureReal();
    return r_Keystroke ? r_Keystroke(i, r, k) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetAudioDeviceIds(DWORD i, LPWSTR a, UINT* ac, LPWSTR b, UINT* bc) {
    EnsureReal();
    return r_AudioIds ? r_AudioIds(i, a, ac, b, bc) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
