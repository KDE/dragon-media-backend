/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once
#include <string_view>

#ifdef _WIN32
#include <windows.h>

namespace DragonThreadName
{
inline void set(std::string_view name)
{
    std::wstring wname(name.begin(), name.end());
    using SetThreadDescriptionFunc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    auto func = reinterpret_cast<SetThreadDescriptionFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (func) {
        func(GetCurrentThread(), wname.c_str());
    }
}
}

#elif defined(__linux__)
#include <pthread.h>
#include <string>

namespace DragonThreadName
{
inline void set(std::string_view name)
{
    pthread_setname_np(pthread_self(), std::string(name).c_str());
}
}
#endif
