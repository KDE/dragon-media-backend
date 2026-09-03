# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_library(dragon_fft_objs OBJECT
    dragonfftframe.cpp
    dragonfftprocessor.cpp
    dragonfftpipeline.cpp
    dragonspectrumanalyzer.cpp
)

target_include_directories(dragon_fft_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_fft_objs PRIVATE
    Qt6::Core
    kissfft::kissfft
    QCoro6::Core
)

target_include_directories(dragon_fft_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../player
)

set_target_properties(dragon_fft_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_fft_objs PRIVATE DragonMediaBackend_EXPORTS)