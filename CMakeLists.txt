cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(s1x_debug_overlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Bake the C++ runtime into the DLL so nothing extra needs installing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# MinHook: a small, well-known library that lets the add-on run a bit of code every frame.
# CMake downloads it automatically during the build; nothing for you to install.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4)
FetchContent_MakeAvailable(minhook)

# Same add-on code, two possible "doors" into the game.
# The player only ever uses ONE of these at a time.
add_library(overlay_version SHARED src/core.cpp src/proxy_version.cpp)
set_target_properties(overlay_version PROPERTIES OUTPUT_NAME "version" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/overlay_out" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/overlay_out" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/overlay_out")
target_compile_definitions(overlay_version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(overlay_dinput8 SHARED src/core.cpp src/proxy_dinput8.cpp)
set_target_properties(overlay_dinput8 PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")
target_compile_definitions(overlay_dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# The separate recorder: same engine, no overlay, only records runs.
add_library(recorder_version SHARED src/core.cpp src/proxy_version.cpp)
set_target_properties(recorder_version PROPERTIES OUTPUT_NAME "version" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/recorder_out" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/recorder_out" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/recorder_out")
target_compile_definitions(recorder_version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX RECORDER_ONLY)
target_link_libraries(recorder_version PRIVATE minhook)

target_link_libraries(overlay_version PRIVATE minhook)
target_link_libraries(overlay_dinput8 PRIVATE minhook)

install(TARGETS overlay_version RUNTIME DESTINATION "try_this_first")
install(TARGETS overlay_dinput8 RUNTIME DESTINATION "try_this_if_first_does_nothing")
install(TARGETS recorder_version RUNTIME DESTINATION "speedrun_recorder")
install(FILES FOR_RUNNERS.txt DESTINATION "speedrun_recorder")
install(FILES INSTALL.txt DESTINATION ".")
