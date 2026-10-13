cmake_minimum_required(VERSION 3.20)
project(fh4_trigger_proxy CXX)

# Name of the XInput DLL to impersonate: xinput1_4 (default), xinput1_3, xinput9_1_0
set(PROXY_NAME "xinput1_4" CACHE STRING "Output DLL name (without .dll)")

# Which build to make: "full" (telemetry + input-only effects + debug tools) or "lite" (telemetry only)
set(FH4_VARIANT "full" CACHE STRING "full or lite")
if(NOT FH4_VARIANT STREQUAL "full" AND NOT FH4_VARIANT STREQUAL "lite")
  message(FATAL_ERROR "FH4_VARIANT must be 'full' or 'lite'")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C/C++ runtime statically so the DLL has no dependency on a VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Games often import XInput by ORDINAL, so exports must use the same numbers
# as the real Microsoft DLLs. Generate the export table per DLL name.
if(PROXY_NAME STREQUAL "xinput9_1_0")
  set(DEF_TEXT
"EXPORTS
    XInputGetCapabilities=Proxy_XInputGetCapabilities @2
    XInputGetDSoundAudioDeviceGuids=Proxy_XInputGetDSoundAudioDeviceGuids @3
    XInputGetState=Proxy_XInputGetState @4
    XInputSetState=Proxy_XInputSetState @5
")
elseif(PROXY_NAME STREQUAL "xinput1_3")
  set(DEF_TEXT
"EXPORTS
    XInputGetState=Proxy_XInputGetState @2
    XInputSetState=Proxy_XInputSetState @3
    XInputGetCapabilities=Proxy_XInputGetCapabilities @4
    XInputEnable=Proxy_XInputEnable @5
    XInputGetDSoundAudioDeviceGuids=Proxy_XInputGetDSoundAudioDeviceGuids @6
    XInputGetBatteryInformation=Proxy_XInputGetBatteryInformation @7
    XInputGetKeystroke=Proxy_XInputGetKeystroke @8
    XInputGetStateEx=Proxy_XInputGetStateEx @100 NONAME
    Proxy_Ordinal101 @101 NONAME
    Proxy_Ordinal102 @102 NONAME
    Proxy_Ordinal103 @103 NONAME
    Proxy_Ordinal104 @104 NONAME
")
else()
  set(DEF_TEXT
"EXPORTS
    XInputGetState=Proxy_XInputGetState @2
    XInputSetState=Proxy_XInputSetState @3
    XInputGetCapabilities=Proxy_XInputGetCapabilities @4
    XInputEnable=Proxy_XInputEnable @5
    XInputGetBatteryInformation=Proxy_XInputGetBatteryInformation @7
    XInputGetKeystroke=Proxy_XInputGetKeystroke @8
    XInputGetAudioDeviceIds=Proxy_XInputGetAudioDeviceIds @10
    XInputGetStateEx=Proxy_XInputGetStateEx @100 NONAME
    Proxy_Ordinal101 @101 NONAME
    Proxy_Ordinal102 @102 NONAME
    Proxy_Ordinal103 @103 NONAME
    Proxy_Ordinal104 @104 NONAME
")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/exports.def" "${DEF_TEXT}")

add_library(proxy SHARED xinput_proxy.cpp "${CMAKE_BINARY_DIR}/exports.def")

set_target_properties(proxy PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PROXY_NAME}")

target_compile_definitions(proxy PRIVATE _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
if(FH4_VARIANT STREQUAL "lite")
  target_compile_definitions(proxy PRIVATE FH4_LITE)
endif()
target_link_libraries(proxy PRIVATE windowsapp user32 ws2_32)
