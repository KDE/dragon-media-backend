# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-3.0-or-later

cmake_minimum_required(VERSION 3.22)

project(dragon-sdl
    VERSION 0.1.0
    DESCRIPTION "KDE audio library using SDL3, ffmpeg, kissfft, and C++23"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX")
endif()

find_package(ECM 6 QUIET)

include(FetchContent)

find_package(Qt6 REQUIRED COMPONENTS Core Network Gui)
find_package(Qt6 COMPONENTS Widgets)
find_package(SDL3 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFmpeg IMPORTED_TARGET REQUIRED
    libavformat
    libavcodec
    libswresample
    libavutil
)

pkg_check_modules(KISSFFT_PC IMPORTED_TARGET kissfft-float)
if(KISSFFT_PC_FOUND)
    message(STATUS "Found kissfft via pkg-config")
    if(NOT TARGET kissfft::kissfft)
        add_library(kissfft::kissfft ALIAS PkgConfig::KISSFFT_PC)
    endif()
else()
    find_package(kissfft CONFIG COMPONENTS float QUIET)
    if(NOT kissfft_FOUND)
        message(STATUS "kissfft not found, downloading...")
        FetchContent_Declare(
            kissfft
            GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
            GIT_TAG 131.2.0
        )
        set(KISSFFT_FLOAT ON CACHE BOOL "Use float for kissfft datatype" FORCE)
        set(KISSFFT_TEST OFF CACHE BOOL "Disable kissfft tests" FORCE)
        set(KISSFFT_TOOLS OFF CACHE BOOL "Disable kissfft tools" FORCE)
        set(KISSFFT_STATIC OFF CACHE BOOL "Disable kissfft static build" FORCE)
        set(KISSFFT_PKGCONFIG OFF CACHE BOOL "Disable kissfft pkgconfig" FORCE)
        FetchContent_MakeAvailable(kissfft)
        if(NOT TARGET kissfft::kissfft AND TARGET kissfft)
            add_library(kissfft::kissfft ALIAS kissfft)
        endif()
    endif()
endif()

FetchContent_Declare(
    spsc_queue_src
    GIT_REPOSITORY https://github.com/joz-k/LockFreeSpscQueue.git
    GIT_TAG v1.0.3
    PATCH_COMMAND git apply --ignore-whitespace ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lockfree_spsc_queue_no_exceptions.patch || true
)
set(SPSC_QUEUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPSC_QUEUE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPSC_QUEUE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SPSC_QUEUE_BUILD_BENCHMARK_COMPARE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spsc_queue_src)

set(CMAKE_AUTOMOC ON)

set(SOURCES
    src/dragonplayer.cpp
    src/dragondecoder.cpp
    src/dragonaudiooutput.cpp
    src/dragonfftprocessor.cpp
    src/dragonradiostream.cpp
    src/dragonicymetadata.cpp
)

set(HEADERS
    include/dragonsdl/dragonplayer.h
    include/dragonsdl/dragondecoder.h
    include/dragonsdl/dragonaudiooutput.h
    include/dragonsdl/dragonradiostream.h
    include/dragonsdl/dragonfftprocessor.h
    include/dragonsdl/dragonfftframe.h
    include/dragonsdl/dragonicymetadata.h
)

qt_add_library(dragon-sdl STATIC ${SOURCES} ${HEADERS})

target_include_directories(dragon-sdl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(dragon-sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(dragon-sdl
    PUBLIC
        Qt6::Core
        Qt6::Network
        SDL3::SDL3
        kissfft::kissfft
        spsc_queue
        PkgConfig::FFmpeg
    PRIVATE
        Qt6::Gui
)

include(GenerateExportHeader)
generate_export_header(dragon-sdl
    BASE_NAME dragonsdl
    EXPORT_MACRO_NAME DRAGONSDL_EXPORT
    EXPORT_FILE_NAME dragonsdl_export.h
)

install(TARGETS dragon-sdl
    EXPORT dragon-sdl-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/dragonsdl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

option(BUILD_EXAMPLES "Build examples" ON)
if(BUILD_EXAMPLES AND Qt6Widgets_FOUND)
    add_subdirectory(examples/qwidget)
endif()

option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Core Test)


    add_executable(test_fftprocessor tests/test_fftprocessor.cpp)
    target_link_libraries(test_fftprocessor PRIVATE
        dragon-sdl
        Qt6::Test
        kissfft::kissfft
        spsc_queue
    )
    add_test(NAME test_fftprocessor COMMAND test_fftprocessor)

    add_executable(test_decoder tests/test_decoder.cpp)
    target_link_libraries(test_decoder PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        PkgConfig::FFmpeg
    )
    add_test(NAME test_decoder COMMAND test_decoder)

    add_executable(test_audiooutput tests/test_audiooutput.cpp)
    target_link_libraries(test_audiooutput PRIVATE
        dragon-sdl
        Qt6::Test
        SDL3::SDL3
        spsc_queue
    )
    add_test(NAME test_audiooutput COMMAND test_audiooutput)

    add_executable(test_radiostream tests/test_radiostream.cpp)
    target_link_libraries(test_radiostream PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
    )
    add_test(NAME test_radiostream COMMAND test_radiostream)

    add_executable(test_player tests/test_player.cpp)
    target_link_libraries(test_player PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_player PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_player COMMAND test_player)

    add_executable(test_e2e tests/test_e2e.cpp)
    target_link_libraries(test_e2e PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        PkgConfig::FFmpeg
    )
    target_compile_definitions(test_e2e PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_e2e COMMAND test_e2e)
endif()
