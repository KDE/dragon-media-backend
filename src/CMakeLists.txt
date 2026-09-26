# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_subdirectory(player)
add_subdirectory(decoder)
add_subdirectory(stream)
add_subdirectory(fft)
add_subdirectory(sink)

include(GenerateExportHeader)

qt_add_library(DragonMediaBackend SHARED
    $<TARGET_OBJECTS:dragon_player_objs>
    $<TARGET_OBJECTS:dragon_decoder_objs>
    $<TARGET_OBJECTS:dragon_stream_objs>
    $<TARGET_OBJECTS:dragon_fft_objs>
    $<TARGET_OBJECTS:dragon_sink_objs>
    ${DragonMediaBackend_SOURCES}
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonplayer.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonaudiooutput.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonduration.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonspectrumanalyzer.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonfftframe.h
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend/dragonicymetadata.h
)

add_library(DragonMediaBackend::DragonMediaBackend ALIAS DragonMediaBackend)

target_include_directories(DragonMediaBackend
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(DragonMediaBackend
    PUBLIC
        Qt6::Core
        Qt6::Network
        PkgConfig::FFmpeg
        QCoro6::Core
    PRIVATE
        Qt6::Gui
        KF6::KIOCore
        KF6::CoreAddons
        KF6::I18n
        kissfft::kissfft
        QCoro6::Network
)

ecm_generate_qdoc(DragonMediaBackend ../docs/dragonmediabackend.qdocconf)
if (TARGET generate_qch_DragonMediaBackend)
    add_dependencies(generate_qch_DragonMediaBackend generate_docs_DragonMediaBackend)
endif()