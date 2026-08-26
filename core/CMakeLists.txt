cmake_minimum_required(VERSION 3.20)
project(mtk-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MTK_BUILD_TESTS "Build the core unit tests" ON)

# The engine is deliberately platform-free: it takes events and a clock and
# returns decisions. Everything that touches an OS API lives in the platform
# library below, behind the interfaces in include/mtk/backends.hpp.
add_library(mtk_engine
    src/keycode.cpp
    src/layer_engine.cpp
    src/motion.cpp
    src/actions.cpp
    src/config.cpp
)
target_include_directories(mtk_engine PUBLIC include)

if(WIN32)
    add_library(mtk_platform
        src/platform/windows/hook_input.cpp
        src/platform/windows/sendinput_output.cpp
        src/platform/windows/win32_window.cpp
        src/platform/windows/scancode_keymap.cpp
    )
    target_link_libraries(mtk_platform PRIVATE user32 dwmapi shcore)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xrandr)

    add_library(mtk_platform
        src/platform/linux/evdev_input.cpp
        src/platform/linux/uinput_output.cpp
        src/platform/linux/x11_window.cpp
        src/platform/linux/evdev_keymap.cpp
    )
    target_link_libraries(mtk_platform PRIVATE PkgConfig::EVDEV PkgConfig::X11)
else()
    message(FATAL_ERROR
        "MouseTrapKeys has no input backend for this platform. "
        "See docs/SPEC.md section 8.")
endif()

target_link_libraries(mtk_platform PUBLIC mtk_engine)

add_executable(mtk-core src/main.cpp src/ipc.cpp)
target_link_libraries(mtk-core PRIVATE mtk_platform)

if(MTK_BUILD_TESTS)
    enable_testing()
    # The engine tests need no display and no privileges, by design.
    add_executable(mtk_engine_tests
        tests/test_layer_engine.cpp
        tests/test_motion.cpp
        tests/test_keycode.cpp
    )
    target_link_libraries(mtk_engine_tests PRIVATE mtk_engine)
    add_test(NAME engine COMMAND mtk_engine_tests)
endif()
