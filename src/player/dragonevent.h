/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstdfloat_compat.h"
#include <QString>
#include <cstdint>
#include <span>
#include <variant>

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