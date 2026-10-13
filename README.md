# FH4 Trigger Proxy
Disclaimer: This project is 100% vibecoded, use at your own will, I tested it and it works for the most part.
I created it because I was bored and curious about Claude AI

Adds impulse-trigger rumble to Forza Horizon 4 (Steam) on Xbox One / Series controllers and
handhelds like the ROG Xbox Ally X.

FH4 only uses XInput to read the controller, so this DLL runs inside the game (Windows only
lets the focused app drive the trigger motors) and generates the effects itself. All XInput
calls are forwarded unchanged to the real Windows `xinput1_4.dll`, and nothing in the game's
files or memory is modified.

## Versions
There are two builds of the same code. Pick one:

| | Lite | Full |
|---|---|---|
| Wheelspin / lock-up effects (needs FH4 Data Out) | yes | yes |
| Input-only effects (main-motor copy, trigger-pull hum and kick) | no | yes |
| Works without Data Out | no | yes |
| Troubleshooting: `log`, `auto_test_seconds` | yes | yes |
| Troubleshooting: `log_input`, `test_combo` | no | yes |
| Settings sections in the ini | General, Telemetry, Debug | General, Telemetry, InputOnly, Debug |

Lite is smaller and only does something while Data Out is on. Full does everything in this README.
Only use one DLL at a time.

## Effects
Everything in this section applies to the **full** build. In the lite build only the Telemetry row exists.

| Mode | When | Right trigger | Left trigger |
|---|---|---|---|
| Telemetry | FH4 Data Out is on | wheelspin under throttle | brake lock-up |
| Input-only | always, unless `with_telemetry=0` (then only when there is no telemetry) | copies the game's right-motor rumble (default), or a hum and kick from throttle pull | copies the game's left-motor rumble (default), or chatter under hard braking |

The two modes are combined: each trigger gets whichever effect is stronger at that moment.
Set `[InputOnly] with_telemetry=0` if you only want the slip effects while Data Out is on.

`[InputOnly] mode` picks what input-only does: `1` (default) copies the game's own main-motor
rumble onto the triggers, `0` reacts to how far you pull them, `2` does both.

## Files
- `xinput_proxy.cpp` - the proxy
- `xinput.def` - export table used by `build.bat` (CMake generates its own per DLL name)
- `CMakeLists.txt` - build project
- `build.bat` - quick local build with cl.exe (xinput1_4 layout only; `build.bat lite` for lite)
- `.github/workflows/build.yml` - builds the DLL in the cloud as `xinput1_4`, `xinput1_3`, `xinput9_1_0`, each in a `full` and a `lite` version

## Build without installing anything
1. Create a free GitHub repo and upload all files, keeping the folder layout (including
   `.github/workflows/build.yml`).
2. Open the **Actions** tab. The "Build proxy DLL" run starts on its own.
3. Open the finished run and download the artifacts. Each is a zip with one DLL, named
   `<dll name>-<version>`, for example `xinput1_3-full` or `xinput1_3-lite`.

FH4 loads **xinput1_3.dll**, so use `xinput1_3-full` or `xinput1_3-lite`.

Local build with CMake: `cmake -B build -A x64 -DPROXY_NAME=xinput1_3 -DFH4_VARIANT=lite`
(`-DFH4_VARIANT=full` is the default), then `cmake --build build --config Release`.

## Install
1. Copy `xinput1_3.dll` (from the `xinput1_3-full` or `xinput1_3-lite` download) next to `ForzaHorizon4.exe`.
2. In Steam, turn off Steam Input for FH4 (Properties > Controller > Disable Steam Input).
3. For wheelspin and lock-up effects (required for the lite build): in FH4 go to Settings > HUD
   and Gameplay and turn **Data Out** on, with IP `127.0.0.1` and port `5300`.
4. Launch the game. `fh4_trigger.ini` is created next to the DLL on first run.

To uninstall, delete the DLL.

## Settings (`fh4_trigger.ini`)
Edit it in Notepad while the game runs; changes apply within about a second. `udp_port` needs
a restart. The file is only created when missing, so delete it to get fresh defaults after an update
or when switching between the lite and full build.

- `[General]` `enabled`, `udp_port`, `log`
- `[Telemetry]` `slip_start`, `slip_full`, `max_strength`, `left_gain`, `right_gain`, `pulse_hz`, `pulse_low`, `min_speed`
- `[InputOnly]` (full only) `enabled`, `with_telemetry`, `mode`, `copy_gain_left`, `copy_gain_right`, `hum_base`, `hum_scale`, `burst_strength`, `burst_threshold`, `burst_window_ms`, `burst_ms`, `chatter_threshold`, `chatter_strength`, `light_brake`

Handy tweaks: raise `left_gain` / `right_gain` for stronger triggers, lower `slip_start` so
wheelspin registers sooner, set `[InputOnly] enabled=0` to turn the generic effect off.

## Troubleshooting (all off by default)
Set these in the ini, then launch the game:

- `log=1` writes `fh4_trigger_proxy.log` next to the DLL, with timestamps. A new log starts at
  each launch. You can switch it on while the game is running, but anything the DLL logs only
  once at startup is missed unless it is on before you launch.
- `[Debug] log_input=1` (full only) also logs what the effects are built from whenever it changes (first
  1000 lines): the trigger values the game passes through XInput, the trigger positions read from
  Windows, and the main-motor rumble the DLL can see.
- `[Debug] auto_test_seconds=25` runs a self-test 25 seconds after the game starts: 1 second on
  the left trigger, 1 on the right, 1 on the main motors, at full strength.
- `[Debug] test_combo=1` (full only) lets you run the same self-test by holding LB + RB + View.

If the triggers stay silent, run the self-test first. If you feel it, the hardware path works
and only the effect tuning needs adjusting.
