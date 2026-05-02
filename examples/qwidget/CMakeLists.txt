# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-3.0-or-later

set(CMAKE_AUTOMOC ON)

add_executable(dragon-sdl-qwidget-example
    main.cpp
    mainwindow.cpp
    mainwindow.h
    dragonplaylist.cpp
    dragonplaylist.h
)

target_link_libraries(dragon-sdl-qwidget-example PRIVATE
    dragon-sdl
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
