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

target_compile_definitions(dragon_sink_objs PRIVATE DragonMultimedia_EXPORTS)