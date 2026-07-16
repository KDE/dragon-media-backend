/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "player/dragonpipe.h"

#include <QtTest>

#include <atomic>
#include <chrono>
#include <ranges>
#include <span>

template<typename T>
T quiescencePolling(std::atomic<T> &counter, int maxWaitMs = 5000, int pollIntervalMs = 20, int stabilityRequired = 2)
{
    T prevValue = counter.load(std::memory_order_relaxed);
    int stableCount = 0;
    for (int elapsed = 0; elapsed < maxWaitMs && stableCount < stabilityRequired; elapsed += pollIntervalMs) {
        QTest::qWait(pollIntervalMs);
        T curValue = counter.load(std::memory_order_relaxed);
        if (curValue == prevValue) {
            ++stableCount;
        } else {
            stableCount = 0;
        }
        prevValue = curValue;
    }
    return prevValue;
}

inline size_t writeAll(DragonPipe<std::float32_t>::Producer producer, std::span<const std::float32_t> data)
{
    return producer.writeSomeWith(data.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        auto in_iter = std::ranges::copy_n(data.begin(), b1.size(), b1.begin()).in;
        std::ranges::copy_n(in_iter, b2.size(), b2.begin());
    });
}

#include "fft/dragonfftblock.h"
inline size_t writeBlocks(DragonPipe<DragonFftBlock>::Producer producer, std::span<const std::float32_t> data)
{
    size_t blocksNeeded = (data.size() + DragonFftBlock::MAX_SAMPLES - 1) / DragonFftBlock::MAX_SAMPLES;
    size_t written = producer.writeSomeWith(blocksNeeded, [&](std::span<DragonFftBlock> b1, std::span<DragonFftBlock> b2) {
        size_t srcOffset = 0;
        size_t blockIdx = 0;
        auto fillDst = [&](std::span<DragonFftBlock> dst) {
            for (size_t i = 0; i < dst.size() && srcOffset < data.size(); ++i, ++blockIdx) {
                size_t toCopy = std::min(data.size() - srcOffset, DragonFftBlock::MAX_SAMPLES);
                dst[i].count = toCopy;
                dst[i].pts = std::chrono::microseconds(blockIdx * 1000);
                for (size_t j = 0; j < toCopy; ++j, ++srcOffset) {
                    dst[i].samples[j] = data[srcOffset];
                }
            }
        };
        fillDst(b1);
        fillDst(b2);
    });
    return written;
}
