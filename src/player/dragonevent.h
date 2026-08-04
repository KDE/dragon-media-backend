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

// A block of decoded PCM samples. `data` views a buffer kept alive by `owner`.
// Consumers must copy what they need and let the chunk go out of scope rather
// than retaining it: the owning buffer is recycled into the decoder's buffer
// pool when the last reference drops, so holding a chunk across threads both
// stalls buffer reuse and runs the pool's deleter off the decode thread.
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
        if (chunk.owner && count > 0) {
            chunk.data = std::span<const std::float32_t>(chunk.owner->data(), count);
        } else {
            chunk.data = {};
        }
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