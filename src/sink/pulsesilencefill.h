/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>

template<typename BeginWriteFn, typename WriteFn, typename CancelWriteFn>
std::size_t fillStreamWithSilence(std::size_t bytes, int channels, BeginWriteFn beginWrite, WriteFn write, CancelWriteFn cancelWrite)
{
    const std::size_t frameBytes = static_cast<std::size_t>(std::max(1, channels)) * sizeof(float);
    std::size_t remainingBytes = bytes;
    while (remainingBytes >= frameBytes) {
        void *buffer = nullptr;
        std::size_t chunkBytes = remainingBytes;
        if (beginWrite(&buffer, &chunkBytes) < 0 || buffer == nullptr || chunkBytes == 0) {
            break;
        }
        const std::size_t writeBytes = std::min(chunkBytes, remainingBytes);
        const std::size_t alignedBytes = writeBytes - (writeBytes % frameBytes);
        if (alignedBytes == 0) {
            cancelWrite();
            break;
        }
        std::memset(buffer, 0, alignedBytes);
        if (write(buffer, alignedBytes) < 0) {
            break;
        }
        remainingBytes -= alignedBytes;
    }
    return bytes - remainingBytes;
}
