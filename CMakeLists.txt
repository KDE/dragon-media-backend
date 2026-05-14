# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(dragon-sdl
    VERSION 0.1.0
    DESCRIPTION "KDE audio library using SDL3, ffmpeg, kissfft, and C++23"
    LANGUAGES CXX
)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache found and enabled")
else()
    message(STATUS "ccache not found")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM 6.0.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(KDEClangFormat)
include(KDEGitCommitHooks)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX")
endif()

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

if(NOT TARGET PkgConfig::FFmpeg)
    add_library(PkgConfig::FFmpeg INTERFACE IMPORTED)
    foreach(_lib IN ITEMS avformat avcodec swresample avutil)
        if(TARGET FFmpeg_${_lib})
            target_link_libraries(PkgConfig::FFmpeg INTERFACE FFmpeg_${_lib})
        endif()
    endforeach()
endif()

pkg_check_modules(KISSFFT_PC IMPORTED_TARGET kissfft-float)
if(KISSFFT_PC_FOUND)
    message(STATUS "Found kissfft via pkg-config")
    if(NOT TARGET kissfft::kissfft)
        add_library(kissfft::kissfft ALIAS PkgConfig::KISSFFT_PC)
    endif()
else()
    find_package(kissfft CONFIG COMPONENTS float QUIET)
    if(NOT TARGET kissfft::kissfft)
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

find_package(QCoro6 QUIET COMPONENTS Core)
if(NOT QCoro6_FOUND)
    message(STATUS "QCoro6 not found, downloading via FetchContent...")
    FetchContent_Declare(
        QCoro
        GIT_REPOSITORY https://github.com/qcoro/qcoro.git
        GIT_TAG v0.13.0
        PATCH_COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-qcoro.sh" "<SOURCE_DIR>"
    )
    block()
        set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "Disable QCoro examples" FORCE)
        set(QCORO_ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disable warnings as errors for QCoro" FORCE)
        set(QCORO_WITH_QTDBUS OFF CACHE BOOL "Disable QCoro QtDBus support" FORCE)
        set(QCORO_WITH_QTNETWORK ON CACHE BOOL "Enable QCoro QtNetwork support" FORCE)
        set(QCORO_WITH_QTSQL OFF CACHE BOOL "Disable QCoro QtSql support" FORCE)
        set(QCORO_WITH_QTWIDGETS OFF CACHE BOOL "Disable QCoro QtWidgets support" FORCE)
        set(QCORO_WITH_QTQUICK OFF CACHE BOOL "Disable QCoro QtQuick support" FORCE)
        set(QCORO_WITH_QTWEBSOCKETS OFF CACHE BOOL "Disable QCoro QtWebSockets support" FORCE)
        set(QCORO_WITH_QML OFF CACHE BOOL "Disable QCoro QML support" FORCE)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "Build QCoro as shared" FORCE)
        set(QCORO_BUILD_TESTING OFF CACHE BOOL "Disable QCoro tests" FORCE)
        set(BUILD_TESTING OFF)
        FetchContent_MakeAvailable(QCoro)
    endblock()
    
    foreach(_qcoro_target IN ITEMS QCoro6Core QCoro6Network)
        if(TARGET ${_qcoro_target})
            target_compile_options(${_qcoro_target} PRIVATE -fexceptions)
            target_compile_options(${_qcoro_target} PRIVATE -Wno-error)
        endif()
    endforeach()
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
#include <generator>
#include <variant>
#include <cstdint>

struct FormatReady { int sampleRate = 0; };
struct SamplesChunk {};
struct DecodeError {};
struct DecodeEof {};
using DecodeEvent = std::variant<FormatReady, SamplesChunk, DecodeError, DecodeEof>;
std::generator<DecodeEvent> test_generator() {
    co_yield FormatReady{44100};
    co_yield DecodeEof{};
}
int main() {
    for (auto e : test_generator()) { (void)e; }
    return 0;
}
" HAVE_CXX23_GENERATOR)

if(NOT HAVE_CXX23_GENERATOR)
    message(FATAL_ERROR "C++23 <generator> header is required but not available. "
                        "Please use GCC >= 16 or Clang with -std=c++23 -stdlib=libc++")
endif()
message(STATUS "C++23 <generator> header: supported")

add_library(spsc_queue INTERFACE)
target_include_directories(spsc_queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/LockFreeSpscQueue>
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_RELY_ON_EXPORTS ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_logging.h"
    IDENTIFIER "dragonsdlPlayer"
    CATEGORY_NAME "org.kde.dragonsdl.player"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_decoder_logging.h"
    IDENTIFIER "dragonsdlDecoder"
    CATEGORY_NAME "org.kde.dragonsdl.decoder"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_audio_logging.h"
    IDENTIFIER "dragonsdlAudio"
    CATEGORY_NAME "org.kde.dragonsdl.audio"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_fft_logging.h"
    IDENTIFIER "dragonsdlFft"
    CATEGORY_NAME "org.kde.dragonsdl.fft"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_network_logging.h"
    IDENTIFIER "dragonsdlNetwork"
    CATEGORY_NAME "org.kde.dragonsdl.network"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdl_SOURCES
    HEADER "dragonsdl_decode_logging.h"
    IDENTIFIER "dragonsdlDecode"
    CATEGORY_NAME "org.kde.dragonsdl.decode"
    DEFAULT_SEVERITY Info
)

set(SOURCES
    src/dragonplayer.cpp
    src/dragondecoder.cpp
    src/dragonaudiooutput.cpp
    src/dragonfftprocessor.cpp
    src/dragonradiostream.cpp
    src/dragonicymetadata.cpp
    src/dragondiagnostics.cpp
    src/dragondecodepipeline.cpp
    src/dragonfftpipeline.cpp
    src/dragonpipe.cpp
    ${dragonsdl_SOURCES}
)

set(HEADERS
    include/dragonsdl/dragonplayer.h
        src/dragondecoder.h
        src/dragonaudiooutput.h
        src/dragonradiostream.h
        src/dragonfftprocessor.h
    include/dragonsdl/dragonfftframe.h
    include/dragonsdl/dragonicymetadata.h
)

qt_add_library(dragon-sdl SHARED ${SOURCES} ${HEADERS})

target_include_directories(dragon-sdl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(dragon-sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(dragon-sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon-sdl
    PUBLIC
        Qt6::Core
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
    PUBLIC
        QCoro6::Core
    PRIVATE
        Qt6::Gui
        kissfft::kissfft
        QCoro6::Network
)

include(GenerateExportHeader)
generate_export_header(dragon-sdl
    BASE_NAME dragonsdl
    EXPORT_MACRO_NAME DRAGONSDL_EXPORT
    EXPORT_FILE_NAME dragonsdl_export.h
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dragonsdl_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dragonsdl
)

install(TARGETS dragon-sdl
    EXPORT dragon-sdl-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/dragonsdl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES dragonsdl.categories DESTINATION ${KDE_INSTALL_DATAROOTDIR}/qlogging-categories6)

include(CMakePackageConfigHelpers)

install(EXPORT dragon-sdl-targets
    FILE dragon-sdl-targets.cmake
    NAMESPACE dragon-sdl::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dragon-sdl
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dragon-sdl-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dragon-sdl-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dragon-sdl
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dragon-sdl-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dragon-sdl-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dragon-sdl-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dragon-sdl
)

file(GLOB ALL_ROOT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
file(GLOB_RECURSE ALL_SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)
file(GLOB_RECURSE ALL_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)
file(GLOB_RECURSE ALL_EXAMPLE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.h"
)
file(GLOB_RECURSE ALL_TEST_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
)
set(ALL_CLANG_FORMAT_SOURCE_FILES
    ${ALL_ROOT_FILES}
    ${ALL_SRC_FILES}
    ${ALL_INCLUDE_FILES}
    ${ALL_EXAMPLE_FILES}
    ${ALL_TEST_FILES}
)

kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

option(BUILD_EXAMPLES "Build examples" ON)
if(BUILD_EXAMPLES AND Qt6Widgets_FOUND)
    add_subdirectory(examples/qwidget)
endif()

option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
    enable_testing()
    include_directories("src")
    find_package(Qt6 REQUIRED COMPONENTS Core Test)


    add_executable(test_fftprocessor tests/test_fftprocessor.cpp)
    target_link_libraries(test_fftprocessor PRIVATE
        dragon-sdl
        Qt6::Test
        kissfft::kissfft
        spsc_queue
    )
    add_test(NAME test_fftprocessor COMMAND test_fftprocessor)

    add_executable(test_generator tests/test_generator.cpp)
    target_link_libraries(test_generator PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(NAME test_generator COMMAND test_generator)

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

    add_executable(test_player_basics tests/test_player_basics.cpp)
    target_link_libraries(test_player_basics PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_player_basics PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_player_basics COMMAND test_player_basics)

    add_executable(test_player_playback tests/test_player_playback.cpp)
    target_link_libraries(test_player_playback PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_player_playback PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_player_playback COMMAND test_player_playback)

    add_executable(test_player_signals tests/test_player_signals.cpp)
    target_link_libraries(test_player_signals PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_player_signals PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_player_signals COMMAND test_player_signals)

    add_executable(test_player_fft tests/test_player_fft.cpp)
    target_link_libraries(test_player_fft PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_player_fft PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_player_fft COMMAND test_player_fft)

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

    add_executable(test_network_playback tests/test_network_playback.cpp)
    target_link_libraries(test_network_playback PRIVATE
        dragon-sdl
        Qt6::Test
        Qt6::Network
        SDL3::SDL3
        PkgConfig::FFmpeg
        spsc_queue
        kissfft::kissfft
    )
    target_compile_definitions(test_network_playback PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME test_network_playback COMMAND test_network_playback)

    add_executable(test_pipe tests/test_pipe.cpp)
    target_link_libraries(test_pipe PRIVATE
        dragon-sdl
        Qt6::Test
        spsc_queue
    )
    add_test(NAME test_pipe COMMAND test_pipe)
endif()
