find_package(Qt6 REQUIRED COMPONENTS Core Test)

function(dragon_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBRARIES;ENVIRONMENT" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE DragonMultimedia Qt6::Test ${ARG_LIBRARIES})
    target_compile_definitions(${name} PRIVATE
        DRAGON_SDL_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
    add_test(NAME ${name} COMMAND ${name})
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
        "QT_PLUGIN_PATH=${PROJECT_BINARY_DIR}/bin"
        ${ARG_ENVIRONMENT}
    )
endfunction()

# Unit tests
dragon_add_test(test_generator LIBRARIES Qt6::Core)
dragon_add_test(test_completion LIBRARIES Qt6::Core)
dragon_add_test(test_fftprocessor LIBRARIES kissfft::kissfft spsc_queue)
dragon_add_test(test_decoder LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_test(test_pipe LIBRARIES spsc_queue)

# Stream tests
dragon_add_test(test_radiostream LIBRARIES Qt6::Network)
dragon_add_test(test_kiostream LIBRARIES Qt6::Network KF6::KIOCore)

# Player tests
set(_player_libs Qt6::Network SDL3::SDL3 PkgConfig::FFmpeg spsc_queue kissfft::kissfft)
dragon_add_test(test_player_basics    LIBRARIES ${_player_libs})
dragon_add_test(test_player_playback  LIBRARIES ${_player_libs})
dragon_add_test(test_player_signals   LIBRARIES ${_player_libs})
dragon_add_test(test_player_prefinish LIBRARIES ${_player_libs})
dragon_add_test(test_player_fft       LIBRARIES ${_player_libs})

# Integration tests
dragon_add_test(test_e2e              LIBRARIES Qt6::Network PkgConfig::FFmpeg)
dragon_add_test(test_network_playback LIBRARIES ${_player_libs})

# PipeWire gapless tests (requires pipewire binary and null-audio-sink)
if(PIPEWIRE_FOUND)
    dragon_add_test(test_e2e_pipewire_gapless
        LIBRARIES ${_player_libs} PkgConfig::PIPEWIRE
        ENVIRONMENT "QT_PLUGIN_PATH=${PROJECT_BINARY_DIR}/bin"
    )
endif()

# Audio output tests (one executable, run once per backend via env var)
dragon_add_test(test_audiooutput LIBRARIES SDL3::SDL3 spsc_queue)
set_tests_properties(test_audiooutput PROPERTIES DISABLED TRUE)

add_test(NAME test_audiooutput_sdl COMMAND test_audiooutput)
set_property(TEST test_audiooutput_sdl APPEND PROPERTY ENVIRONMENT
    "DRAGONMULTIMEDIA_AUDIO_SINK=dragonsdlaudiosink"
    "QT_PLUGIN_PATH=${PROJECT_BINARY_DIR}/bin"
)
if(PULSEAUDIO_FOUND)
    add_test(NAME test_audiooutput_pulseaudio COMMAND test_audiooutput)
    set_property(TEST test_audiooutput_pulseaudio APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpulseaudiosink"
        "QT_PLUGIN_PATH=${PROJECT_BINARY_DIR}/bin"
    )
endif()
if(PIPEWIRE_FOUND)
    add_test(NAME test_audiooutput_pipewire COMMAND test_audiooutput)
    set_property(TEST test_audiooutput_pipewire APPEND PROPERTY ENVIRONMENT
        "DRAGONMULTIMEDIA_AUDIO_SINK=dragonpipewireaudiosink"
        "QT_PLUGIN_PATH=${PROJECT_BINARY_DIR}/bin"
    )
endif()