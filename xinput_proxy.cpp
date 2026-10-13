// FH4 Trigger Proxy
// =================
// In-process XInput proxy that adds impulse-trigger rumble to Forza Horizon 4 (Steam).
//
// FH4 only uses XInput to READ the controller; its rumble goes elsewhere, so it can't be
// intercepted. Instead this DLL runs inside the game (which has window focus, something
// Windows requires before it lets an app drive the trigger motors) and generates the effects:
//
//   Telemetry mode  - when FH4 "Data Out" is on (127.0.0.1:5300) the DLL reads the game's
//                     telemetry:  right trigger = wheelspin, left trigger = brake lock-up.
//   Input-only mode - a generic effect from how far the triggers are pulled. Used when there is
//                     no telemetry, and (by default) blended in alongside it.
//
// Every XInput call is forwarded unchanged to the real system xinput1_4.dll.
// Settings live in fh4_trigger.ini next to this DLL and reload while the game runs.
// Logging and the self-test are for troubleshooting and are OFF by default.

#define PROXY_VERSION "1.1"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <Xinput.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

using namespace winrt::Windows::Gaming::Input;
using winrt::Windows::Foundation::Collections::IVectorView;

// =====================================================================================
// Settings
// =====================================================================================
struct Config {
    // [General]
    bool   enabled = true;
    bool   log = false;               // write fh4_trigger_proxy.log (troubleshooting)
    int    port = 5300;               // restart needed to change

    // [Telemetry]
    double slipStart = 0.35;          // slip ratio where the effect begins
    double slipFull = 1.5;            // slip ratio for full strength
    double maxStrength = 0.9;         // overall cap, 0..1
    double leftGain = 1.0;            // brake trigger multiplier
    double rightGain = 1.0;           // throttle trigger multiplier
    double pulseHz = 14.0;            // buzz rate
    double pulseLow = 0.35;           // strength of the quiet half of each pulse, 0..1
    double minSpeed = 2.0;            // m/s; no effects below this unless braking

    // [InputOnly]
    bool   inputEnabled = true;
    bool   inputWithTelemetry = true; // also apply input-only effects while telemetry is on
    double humBase = 0.15;            // throttle hum at light pull
    double humScale = 0.25;           // extra hum at full pull
    double burstStrength = 0.7;       // kick when the throttle is stabbed
    double burstThreshold = 0.25;     // size of the throttle jump (0..1) that counts as a stab
    int    burstWindowMs = 120;       // ...if it happens within this time
    int    burstMs = 150;             // kick length
    double chatterThreshold = 0.85;   // brake pull where chatter starts
    double chatterStrength = 0.55;
    double lightBrake = 0.15;         // faint buzz at lighter braking

    // [Debug]
    bool   debugInput = false;        // log button/trigger changes
    int    autoTestSeconds = 0;       // run the self-test this long after start (0 = off)
    bool   testCombo = false;         // hold LB + RB + View to run the self-test
};
static Config g_cfg;
static wchar_t g_iniPath[MAX_PATH] = L"fh4_trigger.ini";
static wchar_t g_logPath[MAX_PATH] = L"fh4_trigger_proxy.log";
static FILETIME g_iniTime = {};

// =====================================================================================
// Constants
// =====================================================================================
// FH4 Data Out packet layout (byte offsets)
static const int OFF_RACE_ON    = 0;
static const int OFF_SLIP       = 84;     // 4 floats: FL FR RL RR
static const int OFF_DRIVETRAIN = 224;    // 0 FWD, 1 RWD, 2 AWD
static const int OFF_SPEED      = 256;
static const int OFF_ACCEL      = 315;    // u8
static const int OFF_BRAKE      = 316;    // u8
static const int MIN_PACKET     = 323;

static const ULONGLONG LOOP_MS               = 8;      // loop period / UDP receive timeout
static const ULONGLONG HOUSEKEEPING_MS       = 1000;   // config reload + diagnostics
static const ULONGLONG TELEMETRY_TIMEOUT_MS  = 1000;   // no packets for this long -> input-only
static const ULONGLONG PAD_REFRESH_MS        = 1000;   // re-read the gamepad list this often
static const ULONGLONG STALE_INPUT_MS        = 500;    // XInput values older than this are ignored
static const ULONGLONG TELEMETRY_HOLD_MS     = 100;    // keep the last telemetry effect between packets
static const ULONGLONG SELFTEST_PHASE_MS     = 1000;
static const int       LOG_RESET_GAP_SECONDS = 20;     // start a fresh log if the old one is older
static const int       MAX_INPUT_LOG_LINES   = 200;

static const double TELEMETRY_PEDAL_MIN = 0.2;   // pedal level before slip effects apply
static const double BRAKE_SPEED_OVERRIDE = 0.1;  // braking this much allows effects at low speed
static const double TRIGGER_DEADZONE = 0.1;      // input-only: ignore pulls below this

// =====================================================================================
// Shared state (written by the game's thread via XInputGetState, read by ours)
// =====================================================================================
static volatile LONG      g_throttle = 0;        // right trigger 0..255
static volatile LONG      g_brake = 0;           // left trigger 0..255
static volatile LONG      g_buttons = 0;         // XInput button bitmask
static volatile LONG      g_testRequested = 0;
static volatile ULONGLONG g_lastPoll = 0;        // tick of the game's last successful XInputGetState
static volatile ULONGLONG g_lastTelemetry = 0;   // tick of the last telemetry packet
static LONG               g_activeSlot = -1;     // first XInput slot that reported a controller

// =====================================================================================
// Logging (off unless log=1)
// =====================================================================================
static void Log(const char* fmt, ...) {
    if (!g_cfg.log) return;
    char buf[640];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(buf, 64, "[%04d-%02d-%02d %02d:%02d:%02d.%03d p%lu] ",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, GetCurrentProcessId());
    if (n < 0 || n >= 64) return;
    int room = (int)sizeof(buf) - n - 3;
    va_list a; va_start(a, fmt);
    int m = vsnprintf(buf + n, room, fmt, a);
    va_end(a);
    if (m < 0) return;
    if (m > room) m = room;            // message was truncated
    n += m;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, buf, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

// Steam starts the game exe a few times in quick succession, and they all share one log.
// A launch is "new" when the previous log was last written more than a short while ago.
static void ResetLogIfStale() {
    if (!g_cfg.log) return;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &d)) return;   // no log yet
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now, last;
    now.LowPart = nowFt.dwLowDateTime;               now.HighPart = nowFt.dwHighDateTime;
    last.LowPart = d.ftLastWriteTime.dwLowDateTime;  last.HighPart = d.ftLastWriteTime.dwHighDateTime;
    const ULONGLONG gap = (ULONGLONG)LOG_RESET_GAP_SECONDS * 10000000ULL;      // 100 ns units
    if (now.QuadPart > last.QuadPart + gap) {
        HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);   // truncate
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
}

#define LOG_FIRST() do { if (g_cfg.log) { static LONG c_ = 0; if (InterlockedIncrement(&c_) == 1) Log("first call: %s", __func__); } } while (0)

// =====================================================================================
// Config file
// =====================================================================================
static const char* DEFAULT_INI =
"; FH4 Trigger Proxy settings. Edits apply within about a second while the game runs\r\n"
"; (except udp_port, which needs a restart). Values are 0..1 unless noted.\r\n"
"\r\n"
"[General]\r\n"
"enabled=1            ; 0 turns all trigger effects off\r\n"
"udp_port=5300        ; must match FH4 Settings > HUD and Gameplay > Data Out port\r\n"
"log=0                ; 1 writes fh4_trigger_proxy.log next to this file (troubleshooting)\r\n"
"\r\n"
"; Wheelspin / lock-up effects (needs FH4 Data Out on: IP 127.0.0.1, port above)\r\n"
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
"with_telemetry=1   ; 1 also applies these effects while Data Out is on (0 = slip effects only)\r\n"
"hum_base=0.15        ; throttle hum at light pull\r\n"
"hum_scale=0.25       ; extra hum at full pull\r\n"
"burst_strength=0.7   ; kick when you stab the throttle\r\n"
"burst_threshold=0.25 ; how big the throttle jump must be\r\n"
"burst_window_ms=120  ; ...and how quickly it must happen\r\n"
"burst_ms=150         ; kick length in milliseconds\r\n"
"chatter_threshold=0.85 ; brake pull where chatter starts\r\n"
"chatter_strength=0.55\r\n"
"light_brake=0.15     ; faint buzz at lighter braking\r\n"
"\r\n"
"; Troubleshooting (all off by default)\r\n"
"[Debug]\r\n"
"log_input=0          ; 1 logs button and trigger changes (needs log=1)\r\n"
"auto_test_seconds=0  ; N runs a trigger self-test N seconds after the game starts (0 = off)\r\n"
"test_combo=0         ; 1 lets you run the self-test by holding LB + RB + View\r\n";

static double CfgD(const wchar_t* sec, const wchar_t* key, double def, double lo, double hi) {
    wchar_t dflt[64], buf[64];
    swprintf_s(dflt, L"%g", def);
    GetPrivateProfileStringW(sec, key, dflt, buf, 64, g_iniPath);
    double v = _wtof(buf);             // text after the number (comments) is ignored
    if (!(v == v)) v = def;            // NaN guard
    return std::min(hi, std::max(lo, v));
}
static bool CfgB(const wchar_t* sec, const wchar_t* key, bool def) {
    return CfgD(sec, key, def ? 1 : 0, 0, 1) >= 0.5;
}

static void LoadConfig(bool first) {
    g_cfg.enabled          = CfgB(L"General", L"enabled", true);
    g_cfg.log              = CfgB(L"General", L"log", false);
    if (first) g_cfg.port  = (int)CfgD(L"General", L"udp_port", 5300, 1, 65535);

    g_cfg.slipStart        = CfgD(L"Telemetry", L"slip_start", 0.35, 0, 10);
    g_cfg.slipFull         = CfgD(L"Telemetry", L"slip_full", 1.5, 0.01, 20);
    g_cfg.maxStrength      = CfgD(L"Telemetry", L"max_strength", 0.9, 0, 1);
    g_cfg.leftGain         = CfgD(L"Telemetry", L"left_gain", 1.0, 0, 5);
    g_cfg.rightGain        = CfgD(L"Telemetry", L"right_gain", 1.0, 0, 5);
    g_cfg.pulseHz          = CfgD(L"Telemetry", L"pulse_hz", 14, 0.5, 60);
    g_cfg.pulseLow         = CfgD(L"Telemetry", L"pulse_low", 0.35, 0, 1);
    g_cfg.minSpeed         = CfgD(L"Telemetry", L"min_speed", 2.0, 0, 100);

    g_cfg.inputEnabled     = CfgB(L"InputOnly", L"enabled", true);
    g_cfg.inputWithTelemetry = CfgB(L"InputOnly", L"with_telemetry", true);
    g_cfg.humBase          = CfgD(L"InputOnly", L"hum_base", 0.15, 0, 1);
    g_cfg.humScale         = CfgD(L"InputOnly", L"hum_scale", 0.25, 0, 1);
    g_cfg.burstStrength    = CfgD(L"InputOnly", L"burst_strength", 0.7, 0, 1);
    g_cfg.burstThreshold   = CfgD(L"InputOnly", L"burst_threshold", 0.25, 0.01, 1);
    g_cfg.burstWindowMs    = (int)CfgD(L"InputOnly", L"burst_window_ms", 120, 20, 1000);
    g_cfg.burstMs          = (int)CfgD(L"InputOnly", L"burst_ms", 150, 10, 2000);
    g_cfg.chatterThreshold = CfgD(L"InputOnly", L"chatter_threshold", 0.85, 0, 1);
    g_cfg.chatterStrength  = CfgD(L"InputOnly", L"chatter_strength", 0.55, 0, 1);
    g_cfg.lightBrake       = CfgD(L"InputOnly", L"light_brake", 0.15, 0, 1);

    g_cfg.debugInput       = CfgB(L"Debug", L"log_input", false);
    g_cfg.autoTestSeconds  = (int)CfgD(L"Debug", L"auto_test_seconds", 0, 0, 3600);
    g_cfg.testCombo        = CfgB(L"Debug", L"test_combo", false);
    Log("config loaded (enabled=%d, port=%d)", g_cfg.enabled ? 1 : 0, g_cfg.port);
}

static void EnsureIni() {
    HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;          // already exists, or the folder isn't writable
    DWORD w; WriteFile(h, DEFAULT_INI, (DWORD)strlen(DEFAULT_INI), &w, nullptr);
    CloseHandle(h);
}

static bool IniChanged() {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &d)) return false;
    if (CompareFileTime(&d.ftLastWriteTime, &g_iniTime) == 0) return false;
    g_iniTime = d.ftLastWriteTime;
    return true;
}

// =====================================================================================
// Real XInput (forwarding)
// =====================================================================================
typedef DWORD(WINAPI* GetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* SetState_t)(DWORD, XINPUT_VIBRATION*);
typedef DWORD(WINAPI* GetCaps_t)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void (WINAPI* Enable_t)(BOOL);
typedef DWORD(WINAPI* Battery_t)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD(WINAPI* Keystroke_t)(DWORD, DWORD, PXINPUT_KEYSTROKE);
typedef DWORD(WINAPI* AudioIds_t)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);

static HMODULE     g_real = nullptr;
static GetState_t  r_GetState, r_GetStateEx;
static SetState_t  r_SetState;
static GetCaps_t   r_GetCaps;
static Enable_t    r_Enable;
static Battery_t   r_Battery;
static Keystroke_t r_Keystroke;
static AudioIds_t  r_AudioIds;
static INIT_ONCE   g_realOnce = INIT_ONCE_STATIC_INIT;
static INIT_ONCE   g_threadOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\xinput1_4.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) { Log("failed to load the real xinput1_4.dll"); return TRUE; }
    r_GetState   = (GetState_t)GetProcAddress(g_real, "XInputGetState");
    r_GetStateEx = (GetState_t)GetProcAddress(g_real, MAKEINTRESOURCEA(100));
    r_SetState   = (SetState_t)GetProcAddress(g_real, "XInputSetState");
    r_GetCaps    = (GetCaps_t)GetProcAddress(g_real, "XInputGetCapabilities");
    r_Enable     = (Enable_t)GetProcAddress(g_real, "XInputEnable");
    r_Battery    = (Battery_t)GetProcAddress(g_real, "XInputGetBatteryInformation");
    r_Keystroke  = (Keystroke_t)GetProcAddress(g_real, "XInputGetKeystroke");
    r_AudioIds   = (AudioIds_t)GetProcAddress(g_real, "XInputGetAudioDeviceIds");
    Log("real XInput resolved");
    return TRUE;
}
static void EnsureReal() { InitOnceExecuteOnce(&g_realOnce, InitReal, nullptr, nullptr); }

// =====================================================================================
// Trigger output (Windows.Gaming.Input)
// =====================================================================================
static void NoteGamepadCount(unsigned cnt) {
    static unsigned last = 0xFFFFFFFFu;
    if (cnt == last) return;
    Log("gamepads visible to Windows.Gaming.Input: %u", cnt);
    last = cnt;
}

// Gamepad list shared by the writer and the reader (effects thread only). It is refreshed when
// empty, when it gets old, and after any error.
static IVectorView<Gamepad> g_pads{ nullptr };
static ULONGLONG g_padsTick = 0;

static IVectorView<Gamepad> Pads(ULONGLONG now) {
    if (!g_pads || g_pads.Size() == 0 || now - g_padsTick > PAD_REFRESH_MS) {
        g_pads = Gamepad::Gamepads();
        g_padsTick = now;
        NoteGamepadCount((unsigned)g_pads.Size());
    }
    return g_pads;
}

// Reads the trigger positions straight from Windows (works because the game has focus).
// Unlike the values passed to XInputGetState, these never go stale.
static bool ReadWgiTriggers(ULONGLONG now, double& lt, double& rt) {
    try {
        bool any = false;
        for (auto const& pad : Pads(now)) {
            auto r = pad.GetCurrentReading();
            lt = std::max(lt, r.LeftTrigger);
            rt = std::max(rt, r.RightTrigger);
            any = true;
        }
        return any;
    } catch (...) {
        g_pads = nullptr;
        return false;
    }
}

// Writes trigger strengths to every gamepad Windows.Gaming.Input can see (a handheld may expose
// more than one). motors >= 0 also drives both main motors (self-test only); -1 leaves them alone.
static void ApplyTriggers(double left, double right, double motors = -1.0) {
    static double lastL = -1, lastR = -1, lastM = -1;
    static bool loggedFail = false;

    if (left == 0.0 && right == 0.0 && motors < 0.0 && lastL == 0.0 && lastR == 0.0 && lastM < 0.0)
        return;                                   // already silent

    try {
        for (auto const& pad : Pads(GetTickCount64())) {
            GamepadVibration v{};
            try { v = pad.Vibration(); } catch (...) {}      // keep whatever the main motors are doing
            v.LeftTrigger  = std::min(g_cfg.maxStrength, left * g_cfg.leftGain);
            v.RightTrigger = std::min(g_cfg.maxStrength, right * g_cfg.rightGain);
            if (motors >= 0.0) { v.LeftMotor = motors; v.RightMotor = motors; }
            pad.Vibration(v);
        }
        lastL = left; lastR = right; lastM = motors;
    } catch (...) {
        if (!loggedFail) { Log("trigger write failed"); loggedFail = true; }
        g_pads = nullptr;                         // list may be stale; fetch it again next time
    }
}

static void LogReadback(const char* tag) {
    if (!g_cfg.log) return;
    try {
        unsigned idx = 0;
        for (auto const& pad : Gamepad::Gamepads()) {
            auto v = pad.Vibration();
            Log("%s readback pad %u: LT=%.2f RT=%.2f LM=%.2f RM=%.2f", tag, idx++,
                v.LeftTrigger, v.RightTrigger, v.LeftMotor, v.RightMotor);
        }
    } catch (...) { Log("%s readback failed", tag); }
}

// =====================================================================================
// Effects
// =====================================================================================
struct Effects { double left = 0.0, right = 0.0; };

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

// Wheelspin (right trigger) and lock-up (left trigger) strength from a Data Out packet.
// The buzzing pulse is applied separately, every loop, so it carries on between packets.
static Effects TelemetryAmplitudes(const char* buf) {
    Effects fx;
    if (Rd<int>(buf, OFF_RACE_ON) == 0) return fx;                       // paused / in a menu

    float fl = Rd<float>(buf, OFF_SLIP),     fr = Rd<float>(buf, OFF_SLIP + 4);
    float rl = Rd<float>(buf, OFF_SLIP + 8), rr = Rd<float>(buf, OFF_SLIP + 12);
    int   drive = Rd<int>(buf, OFF_DRIVETRAIN);
    float speed = Rd<float>(buf, OFF_SPEED);
    double accel = (unsigned char)buf[OFF_ACCEL] / 255.0;
    double brake = (unsigned char)buf[OFF_BRAKE] / 255.0;

    if (speed < g_cfg.minSpeed && brake < BRAKE_SPEED_OVERRIDE) return fx;

    double front = std::max(Scale(fl), Scale(fr));
    double rear  = std::max(Scale(rl), Scale(rr));
    double driven = (drive == 0) ? front : (drive == 1) ? rear : std::max(front, rear);
    double lock = std::max(front, rear);

    if (accel > TELEMETRY_PEDAL_MIN) fx.right = driven * accel;
    if (brake > TELEMETRY_PEDAL_MIN) fx.left  = lock * brake;
    return fx;
}

// Generic effect from trigger pull alone (no telemetry).
struct InputFxState { double windowStartT = 0.0; ULONGLONG windowTick = 0, burstUntil = 0; };

static Effects InputOnlyEffects(InputFxState& st, ULONGLONG now, double t, double b) {
    Effects fx;

    if (now - st.windowTick >= (ULONGLONG)g_cfg.burstWindowMs) {         // start a new comparison window
        st.windowStartT = t; st.windowTick = now;
    } else if (t - st.windowStartT > g_cfg.burstThreshold) {             // big rise inside the window
        st.burstUntil = now + (ULONGLONG)g_cfg.burstMs;
        st.windowStartT = t; st.windowTick = now;
    }

    if (now < st.burstUntil)       fx.right = g_cfg.burstStrength;
    else if (t > TRIGGER_DEADZONE) fx.right = g_cfg.humBase + g_cfg.humScale * t;

    if (b > g_cfg.chatterThreshold)     fx.left = g_cfg.chatterStrength * Pulse();
    else if (b > TRIGGER_DEADZONE)      fx.left = g_cfg.lightBrake * b;
    return fx;
}

// Trigger pull 0..1 from the game's own XInput polling (ignored when stale) and from Windows.
static void ReadInputs(ULONGLONG now, double& t, double& b) {
    t = b = 0.0;
    if (now - g_lastPoll <= STALE_INPUT_MS) { t = g_throttle / 255.0; b = g_brake / 255.0; }
    double wl = 0.0, wr = 0.0;
    if (ReadWgiTriggers(now, wl, wr)) { t = std::max(t, wr); b = std::max(b, wl); }
}

// =====================================================================================
// Troubleshooting helpers
// =====================================================================================
// Self-test: 1 s left trigger, 1 s right trigger, 1 s main motors, all at full strength.
struct SelfTest { ULONGLONG start = 0; int phase = -1; };

// Returns true while the test owns the triggers.
static bool RunSelfTest(SelfTest& t, ULONGLONG now) {
    if (!t.start) {
        if (!InterlockedExchange(&g_testRequested, 0)) return false;
        t.start = now; t.phase = -1;
        Log("self-test started");
    }
    int phase = (int)std::min<ULONGLONG>((now - t.start) / SELFTEST_PHASE_MS, 3);
    if (phase >= 3) {
        ApplyTriggers(0.0, 0.0, 0.0);
        t.start = 0;
        Log("self-test finished");
        return true;
    }
    ApplyTriggers(phase == 0 ? 1.0 : 0.0, phase == 1 ? 1.0 : 0.0, phase == 2 ? 0.6 : -1.0);
    if (phase != t.phase) {
        static const char* names[] = { "left trigger", "right trigger", "main motors" };
        Log("self-test: %s", names[phase]);
        LogReadback(names[phase]);
        t.phase = phase;
    }
    return true;
}

struct InputLog { long btn = -1, tb = -1, bb = -1; ULONGLONG last = 0; int count = 0; };

static void LogInputChanges(InputLog& s, ULONGLONG now) {
    if (s.count >= MAX_INPUT_LOG_LINES) return;
    long btn = g_buttons, tb = g_throttle / 64, bb = g_brake / 64;
    if ((btn != s.btn || tb != s.tb || bb != s.bb) && now - s.last >= 250) {
        s.btn = btn; s.tb = tb; s.bb = bb; s.last = now; s.count++;
        Log("input: buttons=0x%04lX throttle=%ld brake=%ld", btn, (long)g_throttle, (long)g_brake);
    }
}

// =====================================================================================
// Background thread: telemetry in, trigger effects out
// =====================================================================================
static SOCKET OpenTelemetrySocket() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return INVALID_SOCKET; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { Log("socket() failed"); return INVALID_SOCKET; }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("bind to 127.0.0.1:%d failed (error %d); running in input-only mode", g_cfg.port, WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }
    DWORD timeoutMs = (DWORD)LOOP_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    Log("listening for telemetry on 127.0.0.1:%d", g_cfg.port);
    return sock;
}

static DWORD WINAPI EffectsThread(LPVOID) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}

    EnsureIni();
    IniChanged();                        // remember the file's current timestamp
    LoadConfig(true);
    SOCKET sock = OpenTelemetrySocket();

    char buf[1024];
    ULONGLONG nextHousekeeping = 0, autoTestAt = 0;
    bool autoTestDone = false, loggedPacket = false, loggedInputMode = false, stalePolling = false;
    Effects telemetryAmp;
    ULONGLONG ampTick = 0;
    InputFxState inputFx;
    SelfTest test;
    InputLog inputLog;

    for (;;) {
        int n = -1;
        if (sock != INVALID_SOCKET) n = recv(sock, buf, sizeof(buf), 0);
        else                        Sleep((DWORD)LOOP_MS);
        ULONGLONG now = GetTickCount64();

        if (now >= nextHousekeeping) {
            nextHousekeeping = now + HOUSEKEEPING_MS;
            if (IniChanged()) LoadConfig(false);
            if (g_cfg.log) {             // diagnostics only
                try { NoteGamepadCount((unsigned)Gamepad::Gamepads().Size()); } catch (...) {}
                bool stale = g_lastPoll != 0 && now - g_lastPoll > STALE_INPUT_MS;
                if (stale != stalePolling) {
                    stalePolling = stale;
                    Log(stale ? "game stopped calling XInputGetState (input read another way?)"
                              : "game is calling XInputGetState again");
                }
            }
        }

        if (!autoTestDone && g_cfg.autoTestSeconds > 0) {
            if (autoTestAt == 0) autoTestAt = now + (ULONGLONG)g_cfg.autoTestSeconds * 1000;
            else if (now >= autoTestAt) { autoTestDone = true; InterlockedExchange(&g_testRequested, 1); }
        }
        if (g_cfg.debugInput) LogInputChanges(inputLog, now);
        if (RunSelfTest(test, now)) continue;

        if (n >= MIN_PACKET) {
            g_lastTelemetry = now;
            if (!loggedPacket) { Log("first telemetry packet, %d bytes", n); loggedPacket = true; }
            telemetryAmp = TelemetryAmplitudes(buf);
            ampTick = now;
        }
        const bool telemetryLive = now - g_lastTelemetry <= TELEMETRY_TIMEOUT_MS;
        const double pulse = Pulse();

        Effects fx;                                                   // slip effects, held between packets
        if (now - ampTick <= TELEMETRY_HOLD_MS) {
            fx.left  = telemetryAmp.left * pulse;
            fx.right = telemetryAmp.right * pulse;
        }
        if (g_cfg.inputEnabled && (!telemetryLive || g_cfg.inputWithTelemetry)) {
            if (!loggedInputMode) {
                Log(telemetryLive ? "input-only effects blended with telemetry" : "no telemetry, using input-only mode");
                loggedInputMode = true;
            }
            double t, b;
            ReadInputs(now, t, b);
            Effects in = InputOnlyEffects(inputFx, now, t, b);
            fx.left  = std::max(fx.left, in.left);
            fx.right = std::max(fx.right, in.right);
        }
        if (!g_cfg.enabled) fx = Effects();
        ApplyTriggers(fx.left, fx.right);
    }
    return 0;
}

static BOOL CALLBACK StartThread(PINIT_ONCE, PVOID, PVOID*) {
    HANDLE h = CreateThread(nullptr, 0, EffectsThread, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); Log("effects thread started"); }
    else   { Log("CreateThread failed"); }
    return TRUE;
}

// Records the controller state the game sees; the first call also starts our thread
// (not from DllMain: the loader lock is held there).
static DWORD Capture(DWORD r, DWORD i, XINPUT_STATE* s) {
    InitOnceExecuteOnce(&g_threadOnce, StartThread, nullptr, nullptr);
    if (r == ERROR_SUCCESS && s) {
        if (g_activeSlot < 0) { g_activeSlot = (LONG)i; Log("controller found on XInput slot %lu", i); }
        if ((LONG)i == g_activeSlot) {
            g_throttle = s->Gamepad.bRightTrigger;
            g_brake    = s->Gamepad.bLeftTrigger;
            g_buttons  = s->Gamepad.wButtons;
            g_lastPoll = GetTickCount64();
            const WORD combo = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_BACK;
            if (g_cfg.testCombo && (s->Gamepad.wButtons & combo) == combo)
                InterlockedExchange(&g_testRequested, 1);
        }
    }
    return r;
}

// =====================================================================================
// Exported functions (names are mapped to the real XInput names by the .def file)
// =====================================================================================
extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
    return Capture(r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED, i, s);
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD i, XINPUT_STATE* s) {
    EnsureReal();
    LOG_FIRST();
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

// Exports that exist in older XInput (1_3 / 9_1_0) but not in 1_4; games linked against
// those DLLs may import them, and a missing export stops the game from starting.
DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD, GUID*, GUID*) { return ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI Proxy_Ordinal101(DWORD, DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal102(DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal103(DWORD) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI Proxy_Ordinal104(DWORD, void*) { return ERROR_CALL_NOT_IMPLEMENTED; }

} // extern "C"

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);

        // Put the log and ini next to this DLL
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) {
                *(slash + 1) = 0;
                wcscpy_s(g_logPath, path);  wcscat_s(g_logPath, L"fh4_trigger_proxy.log");
                wcscpy_s(g_iniPath, path);  wcscat_s(g_iniPath, L"fh4_trigger.ini");
            }
        }

        // Logging is off unless the ini says otherwise; check before writing the first line
        g_cfg.log = CfgB(L"General", L"log", false);
        ResetLogIfStale();

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char exeA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeA, MAX_PATH, nullptr, nullptr);
        Log("FH4 Trigger Proxy " PROXY_VERSION " attached to: %s", exeA);
    }
    return TRUE;
}
