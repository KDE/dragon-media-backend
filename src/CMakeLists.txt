# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_subdirectory(player)
add_subdirectory(decoder)
add_subdirectory(stream)
add_subdirectory(fft)
add_subdirectory(sink)

qt_add_library(DragonMediaBackend SHARED
    ${DragonMediaBackend_SOURCES}
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonplayer.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonaudiooutput.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonspectrumanalyzer.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonfftframe.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonicymetadata.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragoncdtrack.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragoncdtoc.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragoncdtocreader.h
)

add_library(DragonMediaBackend::DragonMediaBackend ALIAS DragonMediaBackend)

set_target_properties(DragonMediaBackend PROPERTIES
    VERSION     ${DRAGONMEDIABACKEND_VERSION}
    SOVERSION   ${DRAGONMEDIABACKEND_SOVERSION}
)

target_include_directories(DragonMediaBackend
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/DragonMediaBackend>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}/DragonMediaBackend>
    PRIVATE
        # The generated qmltyperegistrations.cpp includes the public headers
        # by file name via __has_include().
        ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
)

target_link_libraries(DragonMediaBackend
    PRIVATE
        dragon_player_objs
        dragon_decoder_objs
        dragon_stream_objs
        dragon_fft_objs
        dragon_sink_objs
    PUBLIC
        Qt6::Core
        Qt6::QmlIntegration
)

target_compile_definitions(DragonMediaBackend PRIVATE
    DRAGONMEDIABACKEND_INTERNAL_BUILD
)

# The QML module: the public types self-register into the library via the
# declarative QML macros in their headers; this generates the plugin,
# qmldir and qmltypes for import as "org.kde.dragonmediabackend".
set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(ECMQmlModule)

qt_add_qml_module(DragonMediaBackend
    URI org.kde.dragonmediabackend
    VERSION 1.0
    PLUGIN_TARGET dragonmediabackendplugin
    CLASS_NAME DragonMediaBackendPlugin
    DEPENDENCIES QtCore
)

# KDECMakeSettings empties the prefix of MODULE targets, but the QML
# engine looks for lib<plugin>.so on Unix (ECMQmlModule does the same
# for targets it creates itself).
if(NOT WIN32 AND TARGET dragonmediabackendplugin)
    set_target_properties(dragonmediabackendplugin PROPERTIES PREFIX "lib")
endif()

# The installed plugin lives in <prefix>/<qmldir>/org/kde/dragonmediabackend
# while libDragonMediaBackend.so is installed into <prefix>/<libdir>; give
# the plugin an $ORIGIN-relative rpath so it finds the library.

# Absolute system library directories (e.g. /usr/lib64) are already searched
# by the dynamic linker and distro packaging rejects them as redundant rpaths.
set(_dragon_qml_plugin_install_rpath "")
if(IS_ABSOLUTE "${KDE_INSTALL_QMLDIR}" OR IS_ABSOLUTE "${KDE_INSTALL_LIBDIR}")
    if(NOT KDE_INSTALL_FULL_LIBDIR IN_LIST CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES)
        set(_dragon_qml_plugin_install_rpath "${KDE_INSTALL_FULL_LIBDIR}")
    endif()
else()
    cmake_path(SET _dragon_qml_plugin_libdir "/${KDE_INSTALL_LIBDIR}")
    cmake_path(RELATIVE_PATH _dragon_qml_plugin_libdir
        BASE_DIRECTORY "/${KDE_INSTALL_QMLDIR}/org/kde/dragonmediabackend"
        OUTPUT_VARIABLE _dragon_qml_module_to_libdir
    )
    set(_dragon_qml_plugin_install_rpath "$ORIGIN/${_dragon_qml_module_to_libdir}")
endif()
set_target_properties(dragonmediabackendplugin PROPERTIES
    INSTALL_RPATH "${_dragon_qml_plugin_install_rpath}"
)

ecm_finalize_qml_module(DragonMediaBackend
    DESTINATION ${KDE_INSTALL_QMLDIR}
    VERSION ${PROJECT_VERSION}
)

    ecm_generate_qdoc(DragonMediaBackend ../docs/dragonmediabackend.qdocconf)
if (TARGET generate_qch_DragonMediaBackend)
    add_dependencies(generate_qch_DragonMediaBackend generate_docs_DragonMediaBackend)
endif()