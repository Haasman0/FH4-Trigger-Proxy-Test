# FH4 Trigger Proxy

Adds impulse-trigger rumble to Forza Horizon 4 (Steam) on Xbox One / Series controllers and
handhelds like the ROG Xbox Ally X.

FH4 only uses XInput to read the controller, so this DLL runs inside the game (Windows only
lets the focused app drive the trigger motors) and generates the effects itself. All XInput
calls are forwarded unchanged to the real Windows `xinput1_4.dll`, and nothing in the game's
files or memory is modified.

## Effects

| Mode | When | Right trigger | Left trigger |
|---|---|---|---|
| Telemetry | FH4 Data Out is on | wheelspin under throttle | brake lock-up |
| Input-only | no telemetry for 1 second | hum and a kick when you stab the throttle | chatter under hard braking |

## Files
- `xinput_proxy.cpp` - the proxy
- `xinput.def` - export table used by `build.bat` (CMake generates its own per DLL name)
- `CMakeLists.txt` - build project
- `build.bat` - quick local build with cl.exe (xinput1_4 layout only)
- `.github/workflows/build.yml` - builds the DLL in the cloud as `xinput1_4`, `xinput1_3`, `xinput9_1_0`

## Build without installing anything
1. Create a free GitHub repo and upload all files, keeping the folder layout (including
   `.github/workflows/build.yml`).
2. Open the **Actions** tab. The "Build proxy DLL" run starts on its own.
3. Open the finished run and download the artifacts. Each is a zip with one DLL.

FH4 loads **xinput1_3.dll**, so that is the one to use.

## Install
1. Copy `xinput1_3.dll` next to `ForzaHorizon4.exe`.
2. In Steam, turn off Steam Input for FH4 (Properties > Controller > Disable Steam Input).
3. Optional, for wheelspin and lock-up effects: in FH4 go to Settings > HUD and Gameplay and
   turn **Data Out** on, with IP `127.0.0.1` and port `5300`.
4. Launch the game. `fh4_trigger.ini` is created next to the DLL on first run.

To uninstall, delete the DLL.

## Settings (`fh4_trigger.ini`)
Edit it in Notepad while the game runs; changes apply within about a second. `udp_port` needs
a restart. The file is only created when missing, so delete it to get fresh defaults after an update.

- `[General]` `enabled`, `udp_port`, `log`
- `[Telemetry]` `slip_start`, `slip_full`, `max_strength`, `left_gain`, `right_gain`, `pulse_hz`, `pulse_low`, `min_speed`
- `[InputOnly]` `enabled`, `hum_base`, `hum_scale`, `burst_strength`, `burst_threshold`, `burst_window_ms`, `burst_ms`, `chatter_threshold`, `chatter_strength`, `light_brake`

Handy tweaks: raise `left_gain` / `right_gain` for stronger triggers, lower `slip_start` so
wheelspin registers sooner, set `[InputOnly] enabled=0` to turn the generic effect off.

## Troubleshooting (all off by default)
Set these in the ini, then launch the game:

- `log=1` writes `fh4_trigger_proxy.log` next to the DLL, with timestamps. A new log starts at
  each launch.
- `[Debug] log_input=1` also logs button and trigger changes (first 200).
- `[Debug] auto_test_seconds=25` runs a self-test 25 seconds after the game starts: 1 second on
  the left trigger, 1 on the right, 1 on the main motors, at full strength.
- `[Debug] test_combo=1` lets you run the same self-test by holding LB + RB + View.

If the triggers stay silent, run the self-test first. If you feel it, the hardware path works
and only the effect tuning needs adjusting.
