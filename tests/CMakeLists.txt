# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

find_package(Qt6 REQUIRED COMPONENTS Core Test)

option(DRAGON_CI_PIPEWIRE "Start a headless PipeWire daemon for audio tests" OFF)

function(dragon_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBRARIES;ENVIRONMENT" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE DragonMultimedia Qt6::Test ${ARG_LIBRARIES})
    target_compile_definitions(${name} PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
    add_test(NAME ${name} COMMAND ${name})
    set(_qt_plugin_path "${PROJECT_BINARY_DIR}/bin")
    if(DEFINED ENV{QT_PLUGIN_PATH} AND NOT "$ENV{QT_PLUGIN_PATH}" STREQUAL "")
        string(APPEND _qt_plugin_path ":$ENV{QT_PLUGIN_PATH}")
    endif()
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
        "QT_PLUGIN_PATH=${_qt_plugin_path}"
        ${ARG_ENVIRONMENT}
    )
endfunction()

set(_dragon_audio_tests
    test_player_basics
    test_player_playback
    test_player_signals
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
)

if(DRAGON_CI_PIPEWIRE)
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
dragon_add_test(test_completion LIBRARIES Qt6::Core)
dragon_add_test(test_fftprocessor LIBRARIES kissfft::kissfft spsc_queue)
dragon_add_test(test_decoder LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_test(test_pipe LIBRARIES DragonMultimedia spsc_queue)

# Stream tests
dragon_add_test(test_radiostream LIBRARIES Qt6::Network)
dragon_add_test(test_kiostream LIBRARIES Qt6::Network KF6::KIOCore)

# Player tests
set(_player_libs Qt6::Network SDL3::SDL3 PkgConfig::FFmpeg spsc_queue kissfft::kissfft)
dragon_add_test(test_player_basics    LIBRARIES ${_player_libs})
dragon_add_test(test_player_playback  LIBRARIES ${_player_libs})
dragon_add_test(test_player_signals   LIBRARIES ${_player_libs})
dragon_add_test(test_player_setsource LIBRARIES ${_player_libs})
dragon_add_test(test_player_prefinish LIBRARIES ${_player_libs})
dragon_add_test(test_player_fft       LIBRARIES ${_player_libs})

# Integration tests
dragon_add_test(test_e2e              LIBRARIES Qt6::Network PkgConfig::FFmpeg)
set_tests_properties(test_e2e PROPERTIES DISABLED TRUE)

set(_qt_plugin_path "${PROJECT_BINARY_DIR}/bin")
if(DEFINED ENV{QT_PLUGIN_PATH} AND NOT "$ENV{QT_PLUGIN_PATH}" STREQUAL "")
    string(APPEND _qt_plugin_path ":$ENV{QT_PLUGIN_PATH}")
endif()

add_test(NAME test_e2e_sdl COMMAND test_e2e)
set_property(TEST test_e2e_sdl APPEND PROPERTY ENVIRONMENT
    "DRAGONMULTIMEDIA_AUDIO_SINK=dragonsdlaudiosink"
    "QT_PLUGIN_PATH=${_qt_plugin_path}"
)
if(PULSEAUDIO_FOUND)
    add_test(NAME test_e2e_pulseaudio COMMAND test_e2e)
    set_property(TEST test_e2e_pulseaudio APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpulseaudiosink"
        "QT_PLUGIN_PATH=${_qt_plugin_path}"
    )
endif()
if(PIPEWIRE_FOUND)
    add_test(NAME test_e2e_pipewire COMMAND test_e2e)
    set_property(TEST test_e2e_pipewire APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpipewireaudiosink"
        "QT_PLUGIN_PATH=${_qt_plugin_path}"
    )
endif()

dragon_add_test(test_network_playback LIBRARIES ${_player_libs})

# SDL gapless tests uses SDL disk driver for PCM capture verification
if(FLAC_FOUND)
    dragon_add_test(test_e2e_sdl_gapless LIBRARIES ${_player_libs} PkgConfig::FLAC)
    set_property(TEST test_e2e_sdl_gapless APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonsdlaudiosink"
    )
endif()

# Audio output tests (one executable, run once per backend via env var)
dragon_add_test(test_audiooutput LIBRARIES SDL3::SDL3 spsc_queue Qt6::Gui)
set_tests_properties(test_audiooutput PROPERTIES DISABLED TRUE)

add_test(NAME test_audiooutput_sdl COMMAND test_audiooutput)
set_property(TEST test_audiooutput_sdl APPEND PROPERTY ENVIRONMENT
    "DRAGONMULTIMEDIA_AUDIO_SINK=dragonsdlaudiosink"
    "QT_PLUGIN_PATH=${_qt_plugin_path}"
)
if(PULSEAUDIO_FOUND)
    add_test(NAME test_audiooutput_pulseaudio COMMAND test_audiooutput)
    set_property(TEST test_audiooutput_pulseaudio APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpulseaudiosink"
        "QT_PLUGIN_PATH=${_qt_plugin_path}"
    )
endif()
if(PIPEWIRE_FOUND)
    add_test(NAME test_audiooutput_pipewire COMMAND test_audiooutput)
    set_property(TEST test_audiooutput_pipewire APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpipewireaudiosink"
        "QT_PLUGIN_PATH=${_qt_plugin_path}"
    )
endif()

if(DRAGON_CI_PIPEWIRE)
    foreach(_test ${_dragon_audio_tests})
        if(TEST ${_test})
            set_tests_properties(${_test} PROPERTIES
                FIXTURES_REQUIRED "dragon_pipewire"
            )
        endif()
    endforeach()
endif()
