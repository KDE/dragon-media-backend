# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_sink_objs OBJECT
    dragonaudiosink.cpp
    dragonaudiosinkfactory.cpp
)

target_include_directories(dragon_sink_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_sink_objs PRIVATE
    Qt6::Core
    Qt6::Gui
    KF6::CoreAddons
    QCoro6::Core
)

set_target_properties(dragon_sink_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_sink_objs PRIVATE DragonMediaBackend_EXPORTS)

kcoreaddons_add_plugin(dragonsdlaudiosink
        SOURCES dragonsdlaudiosink.cpp ${dragonsdlaudiosink_SOURCES}
        INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
)
target_include_directories(dragonsdlaudiosink PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)
target_link_libraries(dragonsdlaudiosink PRIVATE DragonMediaBackend SDL3::SDL3 KF6::CoreAddons Qt6::Gui)

if(PULSEAUDIO_FOUND)
    kcoreaddons_add_plugin(dragonpulseaudiosink
            SOURCES dragonpulseaudiosink.cpp ${dragonpulseaudiosink_SOURCES}
            INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
    )
    target_include_directories(dragonpulseaudiosink PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpulseaudiosink PRIVATE DragonMediaBackend PkgConfig::PULSEAUDIO KF6::CoreAddons Qt6::Gui KF6::I18n)
    target_compile_definitions(dragonpulseaudiosink PRIVATE TRANSLATION_DOMAIN="dragonmediabackend")
endif()

if(PIPEWIRE_FOUND)
    kcoreaddons_add_plugin(dragonpipewireaudiosink
            SOURCES dragonpipewireaudiosink.cpp ${dragonpipewireaudiosink_SOURCES}
            INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
    )
    target_include_directories(dragonpipewireaudiosink PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpipewireaudiosink PRIVATE DragonMediaBackend PkgConfig::PIPEWIRE KF6::CoreAddons Qt6::Gui KF6::I18n)
    target_compile_definitions(dragonpipewireaudiosink PRIVATE TRANSLATION_DOMAIN="dragonmediabackend")
endif()
