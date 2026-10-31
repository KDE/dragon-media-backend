# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_mixer_objs OBJECT
    flushtail.cpp
    fadeenvelope.cpp
    fadingchannel.cpp
    mixer.cpp
    primarychannel.cpp
    streamswitch.cpp
)

target_include_directories(dragon_mixer_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_mixer_objs PRIVATE
    Qt6::Core
    Qt6::Gui
    KF6::CoreAddons
    KF6::I18n
)

set_target_properties(dragon_mixer_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_mixer_objs PRIVATE
    DragonMediaBackend_EXPORTS
    TRANSLATION_DOMAIN="dragonmediabackend"
)
