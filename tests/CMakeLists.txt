# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test)

if(NOT WIN32 AND ECM_ENABLE_SANITIZERS MATCHES "thread")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tsan-logs")
    get_directory_property(_ecm_link_libraries LINK_LIBRARIES)
    set_directory_properties(PROPERTIES LINK_LIBRARIES "")
    add_library(tsan_clockjoin_shim MODULE tsan_clockjoin_shim.cpp)
    set_directory_properties(PROPERTIES LINK_LIBRARIES "${_ecm_link_libraries}")
    set_target_properties(tsan_clockjoin_shim PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
    target_compile_options(tsan_clockjoin_shim PRIVATE -fvisibility=default -fno-sanitize=all)
    target_link_options(tsan_clockjoin_shim PRIVATE -fno-sanitize=all)
endif()

# FLAC is a test-only dependency: libFLAC generates the deterministic
# fixtures used by the gapless tests. It is never checked or linked when
# tests are disabled.
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)

include(ECMAddTests)

option(DRAGON_CI_PIPEWIRE "Start a headless PipeWire daemon for audio tests" OFF)
option(DRAGON_CI_NO_AUDIO "Disable tests that require a real audio device (for headless Windows CI)" OFF)

set(_qt_plugin_path "${PROJECT_BINARY_DIR}/bin")
if(DEFINED ENV{QT_PLUGIN_PATH} AND NOT "$ENV{QT_PLUGIN_PATH}" STREQUAL "")
    if(WIN32)
        string(APPEND _qt_plugin_path ";$ENV{QT_PLUGIN_PATH}")
    else()
        string(APPEND _qt_plugin_path ":$ENV{QT_PLUGIN_PATH}")
    endif()
endif()
# needed for Windows:
string(REPLACE ";" "\\;" _qt_plugin_path "${_qt_plugin_path}")

set(_dragon_common_test_env
    "QT_QPA_PLATFORM=offscreen"
    "QT_PLUGIN_PATH=${_qt_plugin_path}"
)

function(dragon_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "LIBRARIES;ENVIRONMENT")
    ecm_add_test(${name}.cpp
        LINK_LIBRARIES DragonMediaBackend Qt6::Test ${ARG_LIBRARIES}
        ENVIRONMENT ${_dragon_common_test_env} ${ARG_ENVIRONMENT}
    )
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
endfunction()

# Adds one disabled "base" test plus <base>_sdl / _pulseaudio / _pipewire
# variants (the latter only when the backend was found).
function(dragon_add_backend_test_variants base)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
        "" "EXTRA_ENV_PULSEAUDIO;EXTRA_ENV_PIPEWIRE")
    set_tests_properties(${base} PROPERTIES DISABLED TRUE)
    add_test(NAME ${base}_sdl COMMAND ${base})
    set_property(TEST ${base}_sdl APPEND PROPERTY ENVIRONMENT
        ${_dragon_common_test_env}
        "DRAGON_AUDIO_SINK=dragonsdlaudiosink"
    )
    if(PULSEAUDIO_FOUND)
        add_test(NAME ${base}_pulseaudio COMMAND ${base})
        set_property(TEST ${base}_pulseaudio APPEND PROPERTY ENVIRONMENT
            ${_dragon_common_test_env}
            "DRAGON_AUDIO_SINK=dragonpulseaudiosink"
            ${ARG_EXTRA_ENV_PULSEAUDIO}
        )
    endif()
    if(PIPEWIRE_FOUND)
        add_test(NAME ${base}_pipewire COMMAND ${base})
        set_property(TEST ${base}_pipewire APPEND PROPERTY ENVIRONMENT
            ${_dragon_common_test_env}
            "DRAGON_AUDIO_SINK=dragonpipewireaudiosink"
            ${ARG_EXTRA_ENV_PIPEWIRE}
        )
    endif()
endfunction()

set(_dragon_audio_tests
    test_player_basics
    test_player_playback_sdl
    test_player_playback_pulseaudio
    test_player_playback_pipewire
    test_player_signals
    test_player_setsource
    test_player_prefinish
    test_player_fft
    test_e2e
    test_e2e_sdl
    test_e2e_pulseaudio
    test_e2e_pipewire
    test_network_playback
    test_audiooutput_sdl
    test_audiooutput_pulseaudio
    test_audiooutput_pipewire
    test_player_streamname_sdl
    test_player_streamname_pulseaudio
    test_player_streamname_pipewire
    test_e2e_gapless_data_streams_sdl
    test_e2e_gapless_data_streams_pulseaudio
    test_e2e_gapless_data_streams_pipewire
    test_cdstream
)

if(DRAGON_CI_PIPEWIRE AND NOT WIN32)
    add_test(NAME pipewire_setup
        COMMAND bash ${CMAKE_SOURCE_DIR}/ci/start-pipewire.sh
                ${CMAKE_SOURCE_DIR}/ci/pipewire-daemon.conf
    )
    set_tests_properties(pipewire_setup PROPERTIES
        FIXTURES_SETUP "dragon_pipewire"
    )

    add_test(NAME pipewire_cleanup
        COMMAND bash -c "killall pipewire wireplumber 2>/dev/null || true"
    )
    set_tests_properties(pipewire_cleanup PROPERTIES
        FIXTURES_CLEANUP "dragon_pipewire"
    )
endif()

# Unit tests
dragon_add_test(test_generator LIBRARIES Qt6::Core)
dragon_add_test(test_completion LIBRARIES Qt6::Core QCoro6::Core)
dragon_add_test(test_fftprocessor LIBRARIES kissfft::kissfft spsc_queue)
dragon_add_test(test_decoder LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_test(test_decoder_fixtures LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_test(test_pipe LIBRARIES DragonMediaBackend spsc_queue)
dragon_add_test(test_positionestimator LIBRARIES Qt6::Core)

# Stream tests
dragon_add_test(test_radiostream LIBRARIES Qt6::Network)
dragon_add_test(test_kiostream LIBRARIES Qt6::Network KF6::KIOCore)

# CD audio tests: run against generated bin/cue images through libcdio's
# bincue driver, so no CD hardware is required.
set(_player_libs Qt6::Network SDL3::SDL3 PkgConfig::FFmpeg spsc_queue kissfft::kissfft)

if(LIBCDIO_FOUND)
    dragon_add_test(test_cdstream LIBRARIES ${_player_libs} KF6::KIOCore)
    dragon_add_test(test_cd_toc LIBRARIES ${_player_libs} Qt6::Qml)
    target_compile_definitions(test_cd_toc PRIVATE
        DRAGON_QML_IMPORT_PATH="${PROJECT_BINARY_DIR}/bin"
    )
endif()

# Player tests
dragon_add_test(test_dragonduration LIBRARIES ${_player_libs})
dragon_add_test(test_dragonduration_qml LIBRARIES ${_player_libs} Qt6::Qml)

# QML module import test: loads org.kde.dragonmediabackend via an import
# path the way the standalone qml6 runtime does (no context properties).
dragon_add_test(test_qml_module LIBRARIES ${_player_libs} Qt6::Qml)
target_compile_definitions(test_qml_module PRIVATE
    DRAGON_QML_IMPORT_PATH="${PROJECT_BINARY_DIR}/bin"
)
dragon_add_test(test_sink_null        LIBRARIES ${_player_libs})
dragon_add_test(test_player_basics    LIBRARIES ${_player_libs})
dragon_add_test(test_player_playback  LIBRARIES ${_player_libs})
dragon_add_backend_test_variants(test_player_playback)
dragon_add_test(test_player_signals   LIBRARIES ${_player_libs})
dragon_add_test(test_player_setsource LIBRARIES ${_player_libs})
dragon_add_test(test_player_prefinish LIBRARIES ${_player_libs})
dragon_add_test(test_player_fft       LIBRARIES ${_player_libs})

# Decode-pipeline gapless promotion race (no audio device needed: the
# pipeline is driven directly, without a sink).
dragon_add_test(test_decodepipeline_promotion LIBRARIES ${_player_libs} QCoro6::Core)

# Integration tests
dragon_add_test(test_e2e              LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_backend_test_variants(test_e2e)

dragon_add_test(test_network_playback LIBRARIES ${_player_libs})

# SDL gapless tests uses SDL disk driver for PCM capture verification
dragon_add_test(test_e2e_sdl_gapless LIBRARIES ${_player_libs} PkgConfig::FLAC)
set_property(TEST test_e2e_sdl_gapless APPEND PROPERTY ENVIRONMENT
    "DRAGON_AUDIO_SINK=dragonsdlaudiosink"
)

dragon_add_test(test_e2e_gapless_data_streams LIBRARIES ${_player_libs} PkgConfig::FLAC)
dragon_add_backend_test_variants(test_e2e_gapless_data_streams
    EXTRA_ENV_PULSEAUDIO "PULSE_SINK=dragon-test-sink"
    EXTRA_ENV_PIPEWIRE "DRAGON_PW_TEST_SINK_NAME=dragon-test-sink"
)

# PipeWire gapless tests requires PipeWire + WirePlumber with dragon-test-sink (monitor ports)
if(PIPEWIRE_FOUND)
    dragon_add_test(test_e2e_pipewire_gapless LIBRARIES ${_player_libs} PkgConfig::FLAC PkgConfig::PIPEWIRE)
    if(DRAGON_CI_PIPEWIRE AND NOT WIN32)
        set_tests_properties(test_e2e_pipewire_gapless PROPERTIES
            FIXTURES_REQUIRED "dragon_pipewire"
        )
    endif()
endif()

# PulseAudio gapless tests Pulse client output routed to dragon-test-sink,
# captured via PipeWire monitor ports (requires PulseAudio on PipeWire)
if(PIPEWIRE_FOUND AND PULSEAUDIO_FOUND)
    dragon_add_test(test_e2e_pulseaudio_gapless LIBRARIES ${_player_libs} PkgConfig::FLAC PkgConfig::PIPEWIRE)
    if(DRAGON_CI_PIPEWIRE AND NOT WIN32)
        set_tests_properties(test_e2e_pulseaudio_gapless PROPERTIES
            FIXTURES_REQUIRED "dragon_pipewire"
        )
    endif()
endif()

# Audio output tests (one executable, run once per backend via env var)
dragon_add_test(test_audiooutput LIBRARIES SDL3::SDL3 spsc_queue Qt6::Gui)
dragon_add_backend_test_variants(test_audiooutput)

# Stream-name player semantics (one executable, run once per backend via env var)
dragon_add_test(test_player_streamname LIBRARIES ${_player_libs} Qt6::Gui)
dragon_add_backend_test_variants(test_player_streamname)

if(DRAGON_CI_PIPEWIRE AND NOT WIN32)
    foreach(_test ${_dragon_audio_tests} test_e2e_pipewire_gapless test_e2e_pulseaudio_gapless)
        if(TEST ${_test})
            set_tests_properties(${_test} PROPERTIES
                FIXTURES_REQUIRED "dragon_pipewire"
            )
        endif()
    endforeach()
endif()

if(DRAGON_CI_NO_AUDIO)
    set(_no_audio_tests ${_dragon_audio_tests} test_e2e_sdl_gapless test_e2e_pipewire_gapless test_e2e_pulseaudio_gapless test_audiooutput test_player_playback)
    foreach(_test ${_no_audio_tests})
        if(TEST ${_test})
            set_tests_properties(${_test} PROPERTIES DISABLED TRUE)
        endif()
    endforeach()
endif()

if(NOT WIN32 AND ECM_ENABLE_SANITIZERS MATCHES "thread")
    get_directory_property(_all_tests TESTS)
    foreach(_test ${_all_tests})
        # Exclude daemon lifecycle fixture scripts from LD_PRELOAD;
        # all actual tests (including test_*_pipewire) are included.
        if(NOT _test STREQUAL "pipewire_setup" AND NOT _test STREQUAL "pipewire_cleanup")
            set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
                "TSAN_OPTIONS=halt_on_error=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-suppressions.txt:log_path=${PROJECT_BINARY_DIR}/tsan-logs/tsan"
                "LD_PRELOAD=$<TARGET_FILE:tsan_clockjoin_shim>"
            )
        endif()
    endforeach()
endif()
