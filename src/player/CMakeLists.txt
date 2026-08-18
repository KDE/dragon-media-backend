# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_player_objs OBJECT
    dragonplayer.cpp
    dragonaudiooutput.cpp
    dragonpipe.cpp
    dragondiagnostics.cpp
)

target_include_directories(dragon_player_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_player_objs PRIVATE
    Qt6::Core
    Qt6::Network
    QCoro6::Core
    KF6::I18n
)

set_target_properties(dragon_player_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_player_objs PRIVATE
    DragonMediaBackend_EXPORTS
    TRANSLATION_DOMAIN="dragonmediabackend"
)