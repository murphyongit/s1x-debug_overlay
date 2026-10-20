cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(s1x_debug_overlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Bake the C++ runtime into the DLL so nothing extra needs installing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Same add-on code, two possible "doors" into the game.
# The player only ever uses ONE of these at a time.
add_library(overlay_version SHARED src/core.cpp src/proxy_version.cpp)
set_target_properties(overlay_version PROPERTIES OUTPUT_NAME "version" PREFIX "")
target_compile_definitions(overlay_version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(overlay_dinput8 SHARED src/core.cpp src/proxy_dinput8.cpp)
set_target_properties(overlay_dinput8 PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")
target_compile_definitions(overlay_dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

install(TARGETS overlay_version RUNTIME DESTINATION "try_this_first")
install(TARGETS overlay_dinput8 RUNTIME DESTINATION "try_this_if_first_does_nothing")
install(FILES INSTALL.txt DESTINATION ".")
