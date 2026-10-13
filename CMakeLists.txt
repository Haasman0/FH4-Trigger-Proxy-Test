cmake_minimum_required(VERSION 3.20)
project(fh4_trigger_proxy CXX)

# Name of the XInput DLL to impersonate. The game loads one of:
#   xinput1_4 (default), xinput1_3, xinput9_1_0
set(PROXY_NAME "xinput1_4" CACHE STRING "Output DLL name (without .dll)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(proxy SHARED xinput_proxy.cpp xinput.def)

set_target_properties(proxy PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PROXY_NAME}")

target_compile_definitions(proxy PRIVATE NOMINMAX)
target_link_libraries(proxy PRIVATE windowsapp user32)
