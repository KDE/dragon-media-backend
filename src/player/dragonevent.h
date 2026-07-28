/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstdfloat_compat.h"
#include <QString>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace DragonMultimedia
{

struct FormatReady {
    int sampleRate = 0;
    int channels = 0;
    int64_t durationMs = -1;
};

struct SamplesChunk {
    std::span<const std::float32_t> data;
    int sampleRate = 0;
    int channels = 0;
    std::shared_ptr<const std::vector<std::float32_t>> owner;

    [[nodiscard]] static SamplesChunk owning(std::shared_ptr<const std::vector<std::float32_t>> buffer, int rate, int ch)
    {
        const size_t count = buffer ? buffer->size() : 0;
        return owning(std::move(buffer), rate, ch, count);
    }

    [[nodiscard]] static SamplesChunk owning(std::shared_ptr<const std::vector<std::float32_t>> buffer, int rate, int ch, size_t count)
    {
        SamplesChunk chunk;
        chunk.owner = std::move(buffer);
        chunk.data = chunk.owner && count > 0 ? std::span<const std::float32_t>(chunk.owner->data(), count) : std::span<const std::float32_t>{};
        chunk.sampleRate = rate;
        chunk.channels = ch;
        return chunk;
    }
};

struct DecodeError {
    QString message;
};

struct DecodeEof {
};

using DecodeEvent = std::variant<FormatReady, SamplesChunk, DecodeError, DecodeEof>;

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}