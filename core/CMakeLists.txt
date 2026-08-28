cmake_minimum_required(VERSION 3.20)
project(keygnosys_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(KGN_BUILD_TESTS "Build the core unit tests" ON)
option(KGN_WERROR     "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------

add_library(kgn_warnings INTERFACE)
if(MSVC)
    target_compile_options(kgn_warnings INTERFACE /W4 /permissive-)
    if(KGN_WERROR)
        target_compile_options(kgn_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(kgn_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(KGN_WERROR)
        target_compile_options(kgn_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# kgn_engine -- the platform-free half
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

add_library(kgn_engine
    src/keycode.cpp
    src/layer_engine.cpp
    src/json.cpp
    src/motion.cpp
    src/actions.cpp
    src/config.cpp
    src/ipc.cpp
    src/hookchannel.cpp
)
target_include_directories(kgn_engine PUBLIC include)
target_link_libraries(kgn_engine PRIVATE kgn_warnings)

# ---------------------------------------------------------------------------
# kgn_platform -- the half that talks to an operating system
#
# One implementation per platform behind the interfaces in
# include/kgn/backends.hpp (principle P5). Not yet built: the sources arrive
# with milestones M3 (Windows) and M4 (Linux/X11). Until then, configuring on
# an unsupported platform is not an error -- there is simply nothing to build.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# kgn_ipc -- endpoint ownership and the platform transports
#
# Separated from kgn_engine because it is the one part of the core that must
# touch an operating system: sockets on Linux, named pipes on Windows, and the
# ownership rules of SPEC 5.1.1-5.1.3 that decide who is allowed to create one.
# The PROTOCOL itself stays in kgn_engine, over the Transport interface, which
# is what lets every envelope, queue and reply-routing rule be tested with no
# socket in sight.
# ---------------------------------------------------------------------------

if(WIN32)
    set(KGN_IPC_SOURCES src/endpoint_windows.cpp)
else()
    set(KGN_IPC_SOURCES src/endpoint_posix.cpp)
endif()
list(APPEND KGN_IPC_SOURCES src/core.cpp)

add_library(kgn_ipc ${KGN_IPC_SOURCES})
target_link_libraries(kgn_ipc PUBLIC kgn_engine PRIVATE kgn_warnings)
if(WIN32)
    # advapi32 carries the token and ACL calls that restrict the pipe to its
    # creating user.
    target_link_libraries(kgn_ipc PRIVATE advapi32)
endif()

set(KGN_PLATFORM_SOURCES "")

if(WIN32)
    set(KGN_PLATFORM_NAME "windows")
    # M3: hook_input.cpp sendinput_output.cpp win32_window.cpp
    #     scancode_keymap.cpp
elseif(UNIX AND NOT APPLE)
    set(KGN_PLATFORM_NAME "linux-x11")
    # M4: evdev_input.cpp uinput_output.cpp x11_window.cpp evdev_keymap.cpp
else()
    set(KGN_PLATFORM_NAME "unsupported")
endif()

if(KGN_PLATFORM_SOURCES)
    add_library(kgn_platform ${KGN_PLATFORM_SOURCES})
    target_link_libraries(kgn_platform PUBLIC kgn_engine PRIVATE kgn_warnings)

    if(WIN32)
        target_link_libraries(kgn_platform PRIVATE user32 dwmapi shcore)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
        pkg_check_modules(X11   REQUIRED IMPORTED_TARGET x11 xrandr)
        target_link_libraries(kgn_platform PRIVATE PkgConfig::EVDEV PkgConfig::X11)
    endif()
else()
    # No backends on this build. backends_none.cpp supplies the factory that
    # backends.hpp declares, returning null members -- which is exactly what
    # that header specifies for a capability this platform does not have. It is
    # NOT a fake backend: a stub that swallowed calls and returned plausible
    # values would let the core look like it was driving the pointer while
    # nothing moved, and P6 exists to forbid precisely that.
    target_sources(kgn_ipc PRIVATE src/backends_none.cpp)
endif()

# ---------------------------------------------------------------------------
# keygnosys-core -- the executable
#
# Built on every platform, backends or not. Its job at M2 is to assemble the
# platform-neutral core and serve IPC; what it cannot do, it says.
# ---------------------------------------------------------------------------

add_executable(keygnosys-core src/main.cpp)
target_link_libraries(keygnosys-core PRIVATE kgn_ipc kgn_warnings)
if(KGN_PLATFORM_SOURCES)
    target_link_libraries(keygnosys-core PRIVATE kgn_platform)
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These need no display, no privileges and no hardware -- that is a design
# constraint on the engine, not a happy accident.
# ---------------------------------------------------------------------------

if(KGN_BUILD_TESTS)
    enable_testing()

    add_library(kgn_test_main INTERFACE)
    target_include_directories(kgn_test_main INTERFACE tests)

    # Most tests need only the platform-free engine. kgn_add_test_with takes
    # the extra libraries for the few that are testing an operating system.
    function(kgn_add_test_with name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE kgn_engine kgn_test_main kgn_warnings ${ARGN})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    function(kgn_add_test name)
        kgn_add_test_with(${name})
    endfunction()

    kgn_add_test(test_keycode)
    kgn_add_test(test_layer_engine)
    kgn_add_test(test_json)
    kgn_add_test(test_motion)
    kgn_add_test(test_actions)
    kgn_add_test(test_config)
    kgn_add_test(test_ipc)
    kgn_add_test(test_hookchannel)
    kgn_add_test_with(test_endpoint kgn_ipc)
    kgn_add_test_with(test_core kgn_ipc)
endif()

# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "KeyGnosys core ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  platform     : ${KGN_PLATFORM_NAME}")
if(NOT KGN_PLATFORM_SOURCES)
    message(STATUS "  backends     : none yet (milestones M3/M4) -- engine and tests only")
endif()
message(STATUS "  tests        : ${KGN_BUILD_TESTS}")
message(STATUS "")
