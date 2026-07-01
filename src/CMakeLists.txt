# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_subdirectory(player)
add_subdirectory(decoder)
add_subdirectory(stream)
add_subdirectory(fft)
add_subdirectory(sink)

include(GenerateExportHeader)

qt_add_library(DragonMultimedia SHARED
    $<TARGET_OBJECTS:dragon_player_objs>
    $<TARGET_OBJECTS:dragon_decoder_objs>
    $<TARGET_OBJECTS:dragon_stream_objs>
    $<TARGET_OBJECTS:dragon_fft_objs>
    $<TARGET_OBJECTS:dragon_sink_objs>
    ${DragonMultimedia_SOURCES}
    ${PROJECT_SOURCE_DIR}/include/DragonMultimedia/dragonplayer.h
    ${PROJECT_SOURCE_DIR}/include/DragonMultimedia/dragonfftframe.h
    ${PROJECT_SOURCE_DIR}/include/DragonMultimedia/dragonicymetadata.h
    ${PROJECT_SOURCE_DIR}/include/DragonMultimedia/dragondiagnostics.h
)

target_include_directories(DragonMultimedia
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(DragonMultimedia
    PUBLIC
        Qt6::Core
        Qt6::Network
        PkgConfig::FFmpeg
        QCoro6::Core
    PRIVATE
        Qt6::Gui
        KF6::KIOCore
        KF6::CoreAddons
        kissfft::kissfft
        QCoro6::Network
)

# --- Audio sink plugins ---
kcoreaddons_add_plugin(dragonsdlaudiosink
    SOURCES sink/dragonsdlaudiosink.cpp ${dragonsdlaudiosink_SOURCES}
    INSTALL_NAMESPACE "DragonMultimedia/audiosink"
)
target_include_directories(dragonsdlaudiosink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)
target_link_libraries(dragonsdlaudiosink PRIVATE DragonMultimedia SDL3::SDL3 KF6::CoreAddons Qt6::Gui)

if(PULSEAUDIO_FOUND)
    kcoreaddons_add_plugin(dragonpulseaudiosink
        SOURCES sink/dragonpulseaudiosink.cpp ${dragonpulseaudiosink_SOURCES}
        INSTALL_NAMESPACE "DragonMultimedia/audiosink"
    )
    target_include_directories(dragonpulseaudiosink PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpulseaudiosink PRIVATE DragonMultimedia PkgConfig::PULSEAUDIO KF6::CoreAddons Qt6::Gui)
endif()

if(PIPEWIRE_FOUND)
    kcoreaddons_add_plugin(dragonpipewireaudiosink
        SOURCES sink/dragonpipewireaudiosink.cpp ${dragonpipewireaudiosink_SOURCES}
        INSTALL_NAMESPACE "DragonMultimedia/audiosink"
    )
    target_include_directories(dragonpipewireaudiosink PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpipewireaudiosink PRIVATE DragonMultimedia PkgConfig::PIPEWIRE KF6::CoreAddons Qt6::Gui)
endif()