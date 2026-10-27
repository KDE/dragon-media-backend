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
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_dragon_android_prefix "$ENV{DRAGON_QT_ANDROID_ARM64}")
    else()
        set(_dragon_android_prefix "$ENV{DRAGON_QT_ANDROID_X86_64}")
    endif()
    if(_dragon_android_prefix STREQUAL "")
        message(FATAL_ERROR
            "DRAGON_QT_ANDROID_ARM64 / DRAGON_QT_ANDROID_X86_64 must point at the "
            "Qt-for-Android craft prefix that provides Qt, FFmpeg and SDL3 "
            "(see ci/craft-android-deps.sh).")
    endif()

    # craft's android build names the library after the ABI
    set(_dragon_sdl3_lib "${_dragon_android_prefix}/lib/libSDL3_${ANDROID_ABI}.so")
    if(NOT EXISTS "${_dragon_sdl3_lib}")
        set(_dragon_sdl3_lib "${_dragon_android_prefix}/lib/libSDL3.so")
    endif()
    file(GLOB _dragon_sdl3_jars LIST_DIRECTORIES false
        "${_dragon_android_prefix}/share/java/SDL3/SDL3-*.jar")
    list(FILTER _dragon_sdl3_jars EXCLUDE REGEX "-sources\\.jar$")
    if(_dragon_sdl3_jars)
        list(GET _dragon_sdl3_jars 0 _dragon_sdl3_jar)
    endif()
    if(NOT EXISTS "${_dragon_sdl3_lib}" OR NOT _dragon_sdl3_jar)
        message(FATAL_ERROR
            "The craft prefix ${_dragon_android_prefix} is missing the SDL3 artifacts.")
    endif()

    set(_dragon_android_pkg_dir ${CMAKE_CURRENT_BINARY_DIR}/android-package)
    file(REMOVE_RECURSE ${_dragon_android_pkg_dir})

    add_custom_target(dragonqml_android_pkg_sync
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${_dragon_android_pkg_dir}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/android ${_dragon_android_pkg_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dragon_android_pkg_dir}/libs
        COMMAND ${CMAKE_COMMAND} -E copy "${_dragon_sdl3_jar}" ${_dragon_android_pkg_dir}/libs
    )
    function(dragonqml_android_pkg_sync_hook)
        if(TARGET dragonqml_example_make_apk)
            add_dependencies(dragonqml_example_make_apk dragonqml_android_pkg_sync)
        else()
            message(FATAL_ERROR
                "dragonqml_example_make_apk target not found — the android "
                "package dir would never be created. Qt changed its internal "
                "apk target names; update the hook in examples/qml/CMakeLists.txt.")
        endif()
    endfunction()
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL dragonqml_android_pkg_sync_hook)

    set_target_properties(dragonqml_example PROPERTIES
        QT_ANDROID_PACKAGE_NAME org.kde.dragonqmlexample
        QT_ANDROID_PACKAGE_SOURCE_DIR ${_dragon_android_pkg_dir}
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_EXTRA_LIBS "${_dragon_sdl3_lib}"
    )
endif()
