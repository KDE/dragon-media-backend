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
    set(_dragon_deps_root "$ENV{DRAGON_ANDROID_DEPS_ROOT}")
    if(_dragon_deps_root STREQUAL "")
        set(_dragon_deps_root "$ENV{HOME}/Android/deps")
    endif()
    set(_dragon_sdl3_jar "${_dragon_deps_root}/sdl3/SDL3-current.jar")
    if(NOT EXISTS "${_dragon_sdl3_jar}")
        message(FATAL_ERROR
            "No SDL3 java glue jar at ${_dragon_sdl3_jar} — "
            "run scripts/build-android-sdl3.sh <abi> first (it builds the jar "
            "from the same research/SDL checkout as libSDL3.so).")
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

    if(ANDROID_ABI STREQUAL arm64-v8a)
        set(_dragon_sdl3_lib "${_dragon_deps_root}/sdl3/arm64-v8a/lib/libSDL3.so")
    else()
        set(_dragon_sdl3_lib "${_dragon_deps_root}/sdl3/x86_64/lib/libSDL3.so")
    endif()
    set(_dragon_sink_lib ${CMAKE_BINARY_DIR}/lib/DragonMediaBackend/AudioSink/libplugins_DragonMediaBackend_AudioSink_dragonsdlaudiosink.so)
    set_target_properties(dragonqml_example PROPERTIES
        QT_ANDROID_PACKAGE_NAME org.kde.dragonqmlexample
        QT_ANDROID_PACKAGE_SOURCE_DIR ${_dragon_android_pkg_dir}
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_EXTRA_LIBS "${_dragon_sdl3_lib};${_dragon_sink_lib}"
    )
endif()
