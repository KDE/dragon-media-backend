# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

set(_dragon_stream_sources
    dragonstream.cpp
    dragonstreamfactory.cpp
    dragonradiostream.cpp
    dragonkiostream.cpp
    dragonbufferprogress.cpp
    dragonicymetadata.cpp
)

if(LIBCDIO_FOUND)
    list(APPEND _dragon_stream_sources dragoncdstream.cpp)
endif()

add_library(dragon_stream_objs OBJECT ${_dragon_stream_sources})

target_include_directories(dragon_stream_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include/DragonMediaBackend
    ${PROJECT_BINARY_DIR}
)

target_link_libraries(dragon_stream_objs PRIVATE
    Qt6::Core
    Qt6::Network
    KF6::KIOCore
    KF6::CoreAddons
    KF6::I18n
    QCoro6::Core
    QCoro6::Network
)

if(LIBCDIO_FOUND)
    target_link_libraries(dragon_stream_objs PRIVATE PkgConfig::LIBCDIO)
    target_compile_definitions(dragon_stream_objs PRIVATE DRAGON_HAVE_LIBCDIO)
    # The libcdio C++ interface signals read errors by throwing
    # DriverOpException; the exceptions are caught inside dragoncdstream.cpp
    # and never escape the translation unit.
    kde_source_files_enable_exceptions(dragoncdstream.cpp)
endif()

set_target_properties(dragon_stream_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_stream_objs PRIVATE
    DragonMediaBackend_EXPORTS
    TRANSLATION_DOMAIN="dragonmediabackend"
)
