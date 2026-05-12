/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonpipe.h"

#include <ranges>
#include <span>

inline size_t writeAll(DragonPipe<std::float32_t>::Producer producer, std::span<const std::float32_t> data)
{
    return producer.writeSomeWith(data.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        auto in_iter = std::ranges::copy_n(data.begin(), b1.size(), b1.begin()).in;
        std::ranges::copy_n(in_iter, b2.size(), b2.begin());
    });
}
