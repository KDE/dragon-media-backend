# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

find_package(Qt6 REQUIRED COMPONENTS Qml Quick QuickControls2)

if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

qt_add_executable(dragonqml_example
    main.cpp
)

if(ANDROID)
    set_target_properties(dragonqml_example PROPERTIES PREFIX "lib")
    target_sources(dragonqml_example PRIVATE
        mediasessioncontroller.h
        mediasessioncontroller.cpp
    )
    target_link_libraries(dragonqml_example PRIVATE SDL3::SDL3)
endif()

qt_add_qml_module(dragonqml_example
    URI org.kde.dragonqmlexample
    VERSION 1.0
    NO_IMPORT_SCAN
    QML_FILES
        Main.qml
        SpectrogramView.qml
    SOURCES
        visualizationcontroller.h
        visualizationcontroller.cpp
        waterfallitem.h
        waterfallitem.cpp
)

target_link_libraries(dragonqml_example PRIVATE
    DragonMediaBackend
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

target_include_directories(dragonqml_example PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/stream
)

if(ANDROID)
    set(_dragon_sdl3_jar "")
    if(TARGET SDL3::Jar)
        get_target_property(_dragon_sdl3_jar SDL3::Jar JAR_FILE)
    endif()

    set(_dragon_android_pkg_dir ${CMAKE_CURRENT_BINARY_DIR}/android-package)
    file(MAKE_DIRECTORY ${_dragon_android_pkg_dir}/libs)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/android/ DESTINATION ${_dragon_android_pkg_dir})
    if(_dragon_sdl3_jar)
        file(COPY ${_dragon_sdl3_jar} DESTINATION ${_dragon_android_pkg_dir}/libs)
    endif()

    add_custom_target(dragonqml_android_pkg_sync
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/android ${_dragon_android_pkg_dir}
        COMMAND ${CMAKE_COMMAND} -E copy "${_dragon_sdl3_jar}" ${_dragon_android_pkg_dir}/libs
    )
    add_dependencies(dragonqml_example dragonqml_android_pkg_sync)

    set_target_properties(dragonqml_example PROPERTIES
        QT_ANDROID_PACKAGE_NAME org.kde.dragonqmlexample
        QT_ANDROID_PACKAGE_SOURCE_DIR ${_dragon_android_pkg_dir}
        QT_ANDROID_MIN_SDK_VERSION 28
    )

    function(dragonqml_make_apk_all)
        if(TARGET dragonqml_example_make_apk)
            add_custom_target(dragonqml_apk ALL DEPENDS dragonqml_example_make_apk)
        endif()
    endfunction()
    cmake_language(DEFER DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} CALL dragonqml_make_apk_all)
endif()
