# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_voice_objs OBJECT
    fadeenvelope.cpp
    voicechannel.cpp
    voice.cpp
)

target_include_directories(dragon_voice_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_voice_objs PRIVATE
    Qt6::Core
    Qt6::Gui
    KF6::CoreAddons
    KF6::I18n
)

set_target_properties(dragon_voice_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_voice_objs PRIVATE
    DragonMediaBackend_EXPORTS
    TRANSLATION_DOMAIN="dragonmediabackend"
)
