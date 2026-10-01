# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

cmake_minimum_required(VERSION 3.25)

project(DragonMediaBackend
    VERSION 0.81.1
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
include(ECMGenerateQDoc)
include(KDEClangFormat)
include(KDEGitCommitHooks)

if(MSVC)
    add_compile_options(/std:c++latest)
endif()

include(FetchContent)

# Allow CI to provide local source clones instead of downloading
if(EXISTS "${CMAKE_SOURCE_DIR}/kissfft-src" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_KISSFFT)
    set(FETCHCONTENT_SOURCE_DIR_KISSFFT "${CMAKE_SOURCE_DIR}/kissfft-src")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/sdl3-src" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_SDL3)
    set(FETCHCONTENT_SOURCE_DIR_SDL3 "${CMAKE_SOURCE_DIR}/sdl3-src")
endif()

# hey libc++ please implement C++23
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
    #include <thread>
    int main() { std::stop_token t; return t.stop_possible() ? 1 : 0; }
" DRAGON_HAVE_STD_STOP_TOKEN)
if(NOT DRAGON_HAVE_STD_STOP_TOKEN)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23 -fexperimental-library")
    check_cxx_source_compiles("
        #include <thread>
        int main() { std::stop_source s; std::jthread j; return 0; }
    " DRAGON_STOP_TOKEN_WITH_EXPERIMENTAL_LIBRARY)
    if(NOT DRAGON_STOP_TOKEN_WITH_EXPERIMENTAL_LIBRARY)
        message(FATAL_ERROR
            "std::stop_token/std::jthread are unavailable and -fexperimental-library "
            "did not enable them — the C++ standard library is too old for this pipeline.")
    endif()
    add_compile_options(-fexperimental-library)
    message(STATUS "std::stop_token needs -fexperimental-library on this toolchain — enabled")
endif()


set(DRAGON_MIN_QT_VERSION 6.11)
find_package(Qt6 ${DRAGON_MIN_QT_VERSION} REQUIRED COMPONENTS Core Network Gui Qml QmlIntegration)
find_package(Qt6 ${DRAGON_MIN_QT_VERSION} COMPONENTS Widgets Quick QuickControls2)
find_package(KF6KIO REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)

if(ANDROID)
    message(STATUS "Android build: SDK=${ANDROID_SDK_ROOT} NDK=${ANDROID_NDK} ABI=${ANDROID_ABI}")
endif()

find_package(SDL3 QUIET)
if(NOT TARGET SDL3::SDL3)
    message(STATUS "SDL3 not found via find_package, trying FetchContent...")
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.12
    )
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    # Only build the audio subsystem dragon-media-backend only uses SDL for audio output
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
    # Disable PCH on MSVC it fails on virtiofs/network drives with C1083
    if(TARGET SDL3-shared)
        set_target_properties(SDL3-shared PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
    endif()
    if(NOT TARGET SDL3::SDL3 AND TARGET SDL3-shared)
        add_library(SDL3::SDL3 ALIAS SDL3-shared)
    endif()
endif()
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFmpeg IMPORTED_TARGET REQUIRED
    libavformat
    libavcodec
    libswresample
    libavutil
)

pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)

pkg_check_modules(PULSEAUDIO IMPORTED_TARGET libpulse)

pkg_check_modules(FLAC IMPORTED_TARGET flac)

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

find_package(QCoro6 QUIET COMPONENTS Core Network)
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
add_library(spsc_queue INTERFACE)
target_include_directories(spsc_queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/LockFreeSpscQueue>
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_RELY_ON_EXPORTS ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_logging.h"
    IDENTIFIER "dragonMediaBackendPlayer"
    CATEGORY_NAME "org.kde.dragonmediabackend.player"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_decoder_logging.h"
    IDENTIFIER "dragonMediaBackendDecoder"
    CATEGORY_NAME "org.kde.dragonmediabackend.decoder"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(dragonsdlaudiosink_SOURCES
    HEADER "dragonmediabackend_audio_logging.h"
    IDENTIFIER "dragonMediaBackendAudio"
    CATEGORY_NAME "org.kde.dragonmediabackend.audio"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_fft_logging.h"
    IDENTIFIER "dragonMediaBackendFft"
    CATEGORY_NAME "org.kde.dragonmediabackend.fft"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_network_logging.h"
    IDENTIFIER "dragonMediaBackendNetwork"
    CATEGORY_NAME "org.kde.dragonmediabackend.network"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_decode_logging.h"
    IDENTIFIER "dragonMediaBackendDecode"
    CATEGORY_NAME "org.kde.dragonmediabackend.decode"
    DEFAULT_SEVERITY Info
)

ecm_qt_declare_logging_category(DragonMediaBackend_SOURCES
    HEADER "dragonmediabackend_factory_logging.h"
    IDENTIFIER "dragonMediaBackendFactory"
    CATEGORY_NAME "org.kde.dragonmediabackend.factory"
    DEFAULT_SEVERITY Info
)

if(PULSEAUDIO_FOUND)
    ecm_qt_declare_logging_category(dragonpulseaudiosink_SOURCES
        HEADER "dragonmediabackend_audio_logging.h"
        IDENTIFIER "dragonMediaBackendAudio"
        CATEGORY_NAME "org.kde.dragonmediabackend.audio"
        DEFAULT_SEVERITY Info
    )
endif()

if(PIPEWIRE_FOUND)
    ecm_qt_declare_logging_category(dragonpipewireaudiosink_SOURCES
        HEADER "dragonmediabackend_audio_logging.h"
        IDENTIFIER "dragonMediaBackendAudio"
        CATEGORY_NAME "org.kde.dragonmediabackend.audio"
        DEFAULT_SEVERITY Info
    )
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_RELY_ON_EXPORTS ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_subdirectory(src)

include(GenerateExportHeader)

generate_export_header(DragonMediaBackend
    BASE_NAME DragonMediaBackend
    EXPORT_MACRO_NAME DRAGONMEDIABACKEND_EXPORT
    EXPORT_FILE_NAME dragonmediabackend_export.h
)

install(TARGETS DragonMediaBackend
    EXPORT DragonMediaBackend-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# DLL hell, not clear if we should do this or not
if(WIN32 AND TARGET SDL3-shared)
    install(TARGETS SDL3-shared
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

set(DragonMediaBackend_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/DragonMediaBackend/dragonplayer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/DragonMediaBackend/dragonaudiooutput.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/DragonMediaBackend/dragonfftframe.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/DragonMediaBackend/dragonicymetadata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/DragonMediaBackend/dragonspectrumanalyzer.h
    ${CMAKE_CURRENT_BINARY_DIR}/dragonmediabackend_export.h
)

install(FILES ${DragonMediaBackend_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/DragonMediaBackend
)

install(FILES dragonmediabackend.categories DESTINATION ${KDE_INSTALL_DATAROOTDIR}/qlogging-categories6)

include(CMakePackageConfigHelpers)

install(EXPORT DragonMediaBackend-targets
    FILE DragonMediaBackend-targets.cmake
    NAMESPACE DragonMediaBackend::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DragonMediaBackend
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DragonMediaBackendConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DragonMediaBackendConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DragonMediaBackend
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/DragonMediaBackendConfig-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/DragonMediaBackendConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/DragonMediaBackendConfig-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DragonMediaBackend
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

if(BUILD_EXAMPLES AND Qt6Qml_FOUND AND Qt6Quick_FOUND AND Qt6QuickControls2_FOUND)
    add_subdirectory(examples/qml)
endif()

option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
