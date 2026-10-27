# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_sink_objs OBJECT
    dragonaudiosink.cpp
    dragonaudiosinkfactory.cpp
    dragonnullaudiosink.cpp
    dragonmediabackend_audio_logging.cpp
)

target_include_directories(dragon_sink_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_sink_objs PRIVATE
    Qt6::Core
    Qt6::Gui
    KF6::CoreAddons
    KF6::I18n
    QCoro6::Core
)

set_target_properties(dragon_sink_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_sink_objs PRIVATE
    DragonMediaBackend_EXPORTS
    TRANSLATION_DOMAIN="dragonmediabackend"
)

if(ANDROID)
    set(DRAGON_SDL_SINK_STATIC STATIC)
endif()
kcoreaddons_add_plugin(dragonsdlaudiosink
        ${DRAGON_SDL_SINK_STATIC}
        SOURCES dragonsdlaudiosink.cpp
        INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
)
target_include_directories(dragonsdlaudiosink PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)
if(ANDROID)
    target_include_directories(dragonsdlaudiosink PRIVATE
            ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
            ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(dragonsdlaudiosink PRIVATE SDL3::SDL3 KF6::CoreAddons Qt6::Gui)
else()
    target_link_libraries(dragonsdlaudiosink PRIVATE DragonMediaBackend SDL3::SDL3 KF6::CoreAddons Qt6::Gui)
endif()

# useful for applications that fetch us via ContentFetch
set(DRAGON_SINK_PLUGIN_BUILD_DIR
    "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/DragonMediaBackend/AudioSink"
    CACHE INTERNAL "Build-tree directory containing the DragonMediaBackend audio sink plugins" FORCE
)

if(PULSEAUDIO_FOUND)
    kcoreaddons_add_plugin(dragonpulseaudiosink
            SOURCES dragonpulseaudiosink.cpp
            INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
    )
    target_include_directories(dragonpulseaudiosink PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_BINARY_DIR}
            ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpulseaudiosink PRIVATE DragonMediaBackend PkgConfig::PULSEAUDIO KF6::CoreAddons Qt6::Gui KF6::I18n)
    target_compile_definitions(dragonpulseaudiosink PRIVATE TRANSLATION_DOMAIN="dragonmediabackend")
endif()

if(PIPEWIRE_FOUND)
    kcoreaddons_add_plugin(dragonpipewireaudiosink
            SOURCES dragonpipewireaudiosink.cpp
            INSTALL_NAMESPACE "DragonMediaBackend/AudioSink"
    )
    target_include_directories(dragonpipewireaudiosink PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_BINARY_DIR}
            ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
    )
    target_link_libraries(dragonpipewireaudiosink PRIVATE DragonMediaBackend PkgConfig::PIPEWIRE KF6::CoreAddons Qt6::Gui KF6::I18n)
    target_compile_definitions(dragonpipewireaudiosink PRIVATE TRANSLATION_DOMAIN="dragonmediabackend")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # PipeWire's SPA headers use C99 compound literals, which Clang warns
        # about in C++ mode (-Wc99-extensions), breaking -Werror builds
        target_compile_options(dragonpipewireaudiosink PRIVATE -Wno-c99-extensions)
    endif()
endif()
