# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-3.0-or-later

# AUTOMOC and INCLUDE_CURRENT_DIR are already set at the top level,
# but we set them explicitly here for clarity and independence from parent.
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(KF6I18n REQUIRED)

add_executable(DragonMultimedia-qwidget-example
    main.cpp
    mainwindow.cpp
    mainwindow.h
    dragonplaylist.cpp
    dragonplaylist.h
    dragonvisualizer.cpp
    dragonvisualizer.h
    dragonspectrogram.cpp
    dragonspectrogram.h
)

target_include_directories(DragonMultimedia-qwidget-example PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/stream
)

target_link_libraries(DragonMultimedia-qwidget-example PRIVATE
    DragonMultimedia
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    KF6::I18n
    KF6::KIOCore
)
