cmake_minimum_required(VERSION 3.31)

project(ClipDeck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

option(CLIPDECK_REQUIRE_GSTREAMER
       "Require GStreamer development packages for the native recorder" ON)

if(CLIPDECK_REQUIRE_GSTREAMER)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-pbutils-1.0
        gio-2.0
        gio-unix-2.0
    )
else()
    pkg_check_modules(GSTREAMER IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-pbutils-1.0
        gio-2.0
        gio-unix-2.0
    )
endif()

set(CLIPDECK_CORE_SOURCES
    src/commands/commands.cpp
    src/core.cpp
    src/feedback_notifier.cpp
    src/save_request_controller.cpp
    src/listener/daemon_listener.cpp
    src/listener/keybind.cpp
    src/listener/keybind_capture.cpp
    src/listener/terminal.cpp
    src/recorder/clip_muxer.cpp
    src/recorder/encoded_ring_buffer.cpp
    src/recorder/gstreamer_recorder.cpp
    src/recorder/media_probe.cpp
    src/recorder/recorder_backend.cpp
    src/recorder/recorder_setup.cpp
    src/recorder/segment_file.cpp
    src/settings/settings_store.cpp
    src/utils/command_runner.cpp
    src/utils/file_descriptor.cpp
    src/utils/logger.cpp
    src/utils/number_parser.cpp
    src/utils/process.cpp
    src/utils/runtime_paths.cpp
    src/utils/string_utils.cpp
)

add_library(clipdeck_core STATIC ${CLIPDECK_CORE_SOURCES})

target_include_directories(clipdeck_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(clipdeck_core
    PUBLIC
        Threads::Threads
)

if(GSTREAMER_FOUND)
    target_compile_definitions(clipdeck_core
        PUBLIC
            CLIPDECK_HAS_GSTREAMER
    )

    target_link_libraries(clipdeck_core
        PUBLIC
            PkgConfig::GSTREAMER
    )
endif()

target_compile_options(clipdeck_core
    PRIVATE
        -Wall
        -Wextra
        -pedantic
)

add_executable(clipdeck
    src/main.cpp
)

target_link_libraries(clipdeck
    PRIVATE
        clipdeck_core
)

if(BUILD_TESTING)
    target_compile_definitions(clipdeck_core
        PUBLIC
            CLIPDECK_ENABLE_RECORDER_TEST_HOOKS
    )

    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        )

        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(clipdeck_tests
        src/tests/clip_muxer_test.cpp
        src/tests/daemon_listener_test.cpp
        src/tests/feedback_notifier_test.cpp
        src/tests/gstreamer_recorder_test.cpp
        src/tests/keybind_test.cpp
        src/tests/encoded_ring_buffer_test.cpp
        src/tests/number_parser_test.cpp
        src/tests/recorder_backend_test.cpp
        src/tests/recorder_setup_test.cpp
        src/tests/runtime_paths_test.cpp
        src/tests/save_request_controller_test.cpp
        src/tests/settings_store_test.cpp
        src/tests/string_utils_test.cpp
    )

    target_link_libraries(clipdeck_tests
        PRIVATE
            clipdeck_core
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(clipdeck_tests)
endif()
