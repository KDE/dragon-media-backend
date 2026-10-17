# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

add_executable(DragonMediaBackend-playcd-example
    main.cpp
)

target_link_libraries(DragonMediaBackend-playcd-example PRIVATE
    DragonMediaBackend
    Qt6::Core
)
