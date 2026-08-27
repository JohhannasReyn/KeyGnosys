cmake_minimum_required(VERSION 3.20)
project(mtk_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MTK_BUILD_TESTS "Build the core unit tests" ON)
option(MTK_WERROR     "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------

add_library(mtk_warnings INTERFACE)
if(MSVC)
    target_compile_options(mtk_warnings INTERFACE /W4 /permissive-)
    if(MTK_WERROR)
        target_compile_options(mtk_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(mtk_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(MTK_WERROR)
        target_compile_options(mtk_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# mtk_engine -- the platform-free half
#
# Touches no OS API, opens no device, starts no thread. It takes events and a
# clock and returns decisions, which is the only reason the logic inherited
# from the prototype is testable at all (SPEC section 13).
#
# Sources are listed explicitly rather than globbed: a glob silently picks up
# whatever is lying in the directory and silently fails to reconfigure when a
# file is added, which is exactly the wrong trade for a build people will run
# rarely and trust completely.
# ---------------------------------------------------------------------------

add_library(mtk_engine
    src/keycode.cpp
    src/layer_engine.cpp
    # Added as milestone M2 lands:
    #   src/motion.cpp         60 Hz pointer/scroll integrator
    #   src/actions.cpp        action catalog and dispatch
    #   src/config.cpp         bindings and settings loading
    #   src/ipc.cpp            JSON Lines server
)
target_include_directories(mtk_engine PUBLIC include)
target_link_libraries(mtk_engine PRIVATE mtk_warnings)

# ---------------------------------------------------------------------------
# mtk_platform -- the half that talks to an operating system
#
# One implementation per platform behind the interfaces in
# include/mtk/backends.hpp (principle P5). Not yet built: the sources arrive
# with milestones M3 (Windows) and M4 (Linux/X11). Until then, configuring on
# an unsupported platform is not an error -- there is simply nothing to build.
# ---------------------------------------------------------------------------

set(MTK_PLATFORM_SOURCES "")

if(WIN32)
    set(MTK_PLATFORM_NAME "windows")
    # M3: hook_input.cpp sendinput_output.cpp win32_window.cpp
    #     scancode_keymap.cpp
elseif(UNIX AND NOT APPLE)
    set(MTK_PLATFORM_NAME "linux-x11")
    # M4: evdev_input.cpp uinput_output.cpp x11_window.cpp evdev_keymap.cpp
else()
    set(MTK_PLATFORM_NAME "unsupported")
endif()

if(MTK_PLATFORM_SOURCES)
    add_library(mtk_platform ${MTK_PLATFORM_SOURCES})
    target_link_libraries(mtk_platform PUBLIC mtk_engine PRIVATE mtk_warnings)

    if(WIN32)
        target_link_libraries(mtk_platform PRIVATE user32 dwmapi shcore)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
        pkg_check_modules(X11   REQUIRED IMPORTED_TARGET x11 xrandr)
        target_link_libraries(mtk_platform PRIVATE PkgConfig::EVDEV PkgConfig::X11)
    endif()

    add_executable(mtk-core src/main.cpp)
    target_link_libraries(mtk-core PRIVATE mtk_platform mtk_warnings)
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These need no display, no privileges and no hardware -- that is a design
# constraint on the engine, not a happy accident.
# ---------------------------------------------------------------------------

if(MTK_BUILD_TESTS)
    enable_testing()

    add_library(mtk_test_main INTERFACE)
    target_include_directories(mtk_test_main INTERFACE tests)

    function(mtk_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE mtk_engine mtk_test_main mtk_warnings)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    mtk_add_test(test_keycode)
    mtk_add_test(test_layer_engine)
    # Added with M2:
    #   mtk_add_test(test_motion)         ramp, diagonals, pixel accumulation
endif()

# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "MouseTrapKeys core ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  platform     : ${MTK_PLATFORM_NAME}")
if(NOT MTK_PLATFORM_SOURCES)
    message(STATUS "  backends     : none yet (milestones M3/M4) -- engine and tests only")
endif()
message(STATUS "  tests        : ${MTK_BUILD_TESTS}")
message(STATUS "")
