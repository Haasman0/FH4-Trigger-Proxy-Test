@echo off
REM Run from "x64 Native Tools Command Prompt for VS" (Visual Studio Build Tools + Windows SDK).
REM FH4 is 64-bit, so build x64.
REM Output name must match the XInput DLL the game loads. Default: xinput1_4.dll.
REM If the log never appears, try xinput1_3 or xinput9_1_0 (edit OUT below).

set OUT=xinput1_4.dll

cl /nologo /std:c++17 /EHsc /O2 /LD xinput_proxy.cpp ^
   /link /DEF:xinput.def /OUT:%OUT% windowsapp.lib user32.lib

if exist %OUT% (
  echo.
  echo Built %OUT%
  echo Copy it into the Forza Horizon 4 folder next to ForzaHorizon4.exe
) else (
  echo Build failed
)
