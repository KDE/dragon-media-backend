/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

template<typename T>
class Pipe;

/**
 * A "stream switch" is either a crossfade handoff or a flush (seek / hard
 * switch / crossfade abort). Exactly one request can be outstanding:
 * stage() refuses while a previous request has not been consumed, and
 * supersede() atomically voids the outstanding request
 *
 * Implementation is a seqlock.
 *
 * Threading contract:
 *  - stage()/supersede()/clear()/isPending()/hasFadingPipe(): Qt main thread,
 *  - poll()/take()/acknowledge(): audio callback thread (single consumer),
 *  - setAckedCallback(): once, before audio starts; invoked from acknowledge()
 *    on the audio thread after a successful ack. Must be cheap and lock-free.
 */
class DRAGONMEDIABACKEND_EXPORT StreamSwitch
{
public:
    enum class Kind : std::uint8_t {
        None,
        Flush,
        Crossfade,
    };

    struct Request {
        Kind kind = Kind::None;
        std::uint64_t epoch = 0; // epoch minted at staging time
        Pipe<float> *primaryPipe = nullptr; // pipe to install
        Pipe<float> *fadingPipe = nullptr; // pipe to fade out
        std::chrono::milliseconds fadeOut{0};
        std::chrono::milliseconds fadeIn{0};
    };

    StreamSwitch() = default;
    ~StreamSwitch() = default;

    StreamSwitch(const StreamSwitch &) = delete;
    StreamSwitch &operator=(const StreamSwitch &) = delete;
    StreamSwitch(StreamSwitch &&) = delete;
    StreamSwitch &operator=(StreamSwitch &&) = delete;

    // Main thread
    [[nodiscard]] bool stage(const Request &request) noexcept;

    // Main thread
    [[nodiscard]] Request supersede(const Request &replacement) noexcept;

    // Main thread
    [[nodiscard]] Request clear() noexcept;

    // Main thread
    [[nodiscard]] bool isPending() const noexcept;
    // Main thread
    [[nodiscard]] Request peek() const noexcept;
    [[nodiscard]] bool hasFadingPipe(const Pipe<float> *pipe) const noexcept;

    // Audio thread
    [[nodiscard]] bool poll() const noexcept;

    // Audio thread.
    [[nodiscard]] Request take() noexcept;
    void acknowledge() noexcept;

    // Install once, before any audio thread runs. Invoked from acknowledge()
    // on the audio thread after releasing the claim.
    void setAckedCallback(std::function<void()> callback);

private:
    void writeRequest(const Request &request) noexcept;
    [[nodiscard]] Request readRequest() const noexcept;

    std::atomic<std::uint64_t> m_seq{0}; // even = stable; each staged request takes a new even value
    std::atomic<std::uint64_t> m_pendingSeq{0}; // 0 = idle; else the sequence of the outstanding request (odd = claimed by the consumer)
    std::uint64_t m_claimedSeq = 0; // audio-thread only: the sequence claimed by take() and not yet acknowledged

    // Seqlock-protected payload.
    std::atomic<std::uint8_t> m_kind{0};
    std::atomic<std::uint64_t> m_epoch{0};
    std::atomic<Pipe<float> *> m_primaryPipe{nullptr};
    std::atomic<Pipe<float> *> m_fadingPipe{nullptr};
    std::atomic<std::int64_t> m_fadeOutMs{0};
    std::atomic<std::int64_t> m_fadeInMs{0};

    std::function<void()> m_ackedCallback;
};
