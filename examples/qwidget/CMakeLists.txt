# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# AUTOMOC and INCLUDE_CURRENT_DIR are already set at the top level,
# but we set them explicitly here for clarity and independence from parent.
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(KF6I18n REQUIRED)

if(NOT TARGET QCoro6::Core)
    find_package(QCoro6 REQUIRED COMPONENTS Core)
endif()

find_package(KCddb6 QUIET)
if(NOT KCddb6_FOUND)
    message(STATUS "KCddb6 not found, downloading via FetchContent...")
    include(FetchContent)

    FetchContent_Declare(
        kcddb
        GIT_REPOSITORY https://invent.kde.org/multimedia/libkcddb.git
        GIT_TAG 13d4b2322038bdc2d492eb37fb512822907aaa88
        EXCLUDE_FROM_ALL
    )
    set(BUILD_DOC OFF)
    set(BUILD_TESTING OFF)
    FetchContent_MakeAvailable(kcddb)
endif()

add_executable(DragonMediaBackend-qwidget-example
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

target_include_directories(DragonMediaBackend-qwidget-example PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/stream
)

target_link_libraries(DragonMediaBackend-qwidget-example PRIVATE
    DragonMediaBackend
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    KF6::I18n
    QCoro6::Core
    KCddb6
)
