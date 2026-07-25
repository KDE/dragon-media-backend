/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once
#include <version>
#if __has_include(<stdfloat>)
#include <stdfloat>
#endif
#ifndef _DRAGON_STDFLOAT_COMPAT
#define _DRAGON_STDFLOAT_COMPAT
namespace std
{
#if !defined(__cpp_lib_extended_floatpoint_types) && (defined(_MSC_VER) || !__has_include(<stdfloat>))
using float32_t = float;
#endif
}
#endif
