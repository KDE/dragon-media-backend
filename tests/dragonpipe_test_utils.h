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

#include "dragonfftblock.h"
inline size_t writeBlocks(DragonPipe<DragonFftBlock>::Producer producer, std::span<const std::float32_t> data)
{
    size_t blocksNeeded = (data.size() + DragonFftBlock::MAX_SAMPLES - 1) / DragonFftBlock::MAX_SAMPLES;
    return producer.writeSomeWith(blocksNeeded, [&](std::span<DragonFftBlock> b1, std::span<DragonFftBlock> b2) {
        size_t srcOffset = 0;
        auto fillDst = [&](std::span<DragonFftBlock> dst) {
            for (size_t i = 0; i < dst.size() && srcOffset < data.size(); ++i) {
                size_t toCopy = std::min(data.size() - srcOffset, DragonFftBlock::MAX_SAMPLES);
                dst[i].count = toCopy;
                dst[i].pts = std::chrono::microseconds(1000);
                for (size_t j = 0; j < toCopy; ++j, ++srcOffset) {
                    dst[i].samples[j] = data[srcOffset];
                }
            }
        };
        fillDst(b1);
        fillDst(b2);
    });
}
