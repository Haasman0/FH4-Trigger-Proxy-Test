# FH4 Trigger Rumble Proxy

XInput proxy DLL that forwards all calls to the real Windows XInput and also
mirrors the game's rumble onto the Xbox impulse triggers via
Windows.Gaming.Input. Built for the Steam version of Forza Horizon 4.

## Files
- `xinput_proxy.cpp` - the proxy
- `xinput.def` - export table (includes ordinal 100, XInputGetStateEx)
- `CMakeLists.txt` - build project
- `build.bat` - quick build with cl.exe (alternative to CMake)
- `.github/workflows/build.yml` - builds the DLL in the cloud

## Option A: build in the cloud (no tools to install)
1. Create a free GitHub repo and upload all files, keeping the folder layout
   (including `.github/workflows/build.yml`).
2. Open the repo's **Actions** tab. The "Build proxy DLL" run starts on its own.
3. When it finishes, open the run and download the artifacts:
   `xinput1_4`, `xinput1_3`, `xinput9_1_0`. Each is a zip with one DLL.

## Option B: build locally
Install Visual Studio Build Tools (C++ workload + Windows SDK), then in an
"x64 Native Tools Command Prompt":

    cmake -B build -A x64
    cmake --build build --config Release

The DLL is `build\Release\xinput1_4.dll`. To pick another name:

    cmake -B build -A x64 -DPROXY_NAME=xinput1_3

## Install
1. Copy ONE DLL next to `ForzaHorizon4.exe` (start with `xinput1_4.dll`).
2. In Steam, disable Steam Input for FH4.
3. Play, then check `fh4_trigger_proxy.log` in the game folder:
   - lines with `XInputSetState` = working
   - no log = the game loads a different XInput DLL; swap in `xinput1_3.dll`
     or `xinput9_1_0.dll` (only one at a time)
   - log but no `XInputSetState` lines = the game doesn't rumble through XInput

## Uninstall
Delete the DLL from the game folder.

## Tuning
Edit `LEFT_TRIGGER_GAIN` and `RIGHT_TRIGGER_GAIN` at the top of
`xinput_proxy.cpp` and rebuild.
