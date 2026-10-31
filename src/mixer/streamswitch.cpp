/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "streamswitch.h"

void StreamSwitch::setAckedCallback(std::function<void()> callback)
{
    m_ackedCallback = std::move(callback);
}

void StreamSwitch::writeRequest(const Request &request) noexcept
{
    const auto seq = m_seq.load(std::memory_order_relaxed);
    m_seq.store(seq + 1, std::memory_order_release); // odd: payload is being written
    m_kind.store(static_cast<std::uint8_t>(request.kind), std::memory_order_relaxed);
    m_epoch.store(request.epoch, std::memory_order_relaxed);
    m_primaryPipe.store(request.primaryPipe, std::memory_order_relaxed);
    m_fadingPipe.store(request.fadingPipe, std::memory_order_relaxed);
    m_fadeOutMs.store(request.fadeOut.count(), std::memory_order_relaxed);
    m_fadeInMs.store(request.fadeIn.count(), std::memory_order_relaxed);
    m_seq.store(seq + 2, std::memory_order_release); // even: payload is stable
}

StreamSwitch::Request StreamSwitch::readRequest() const noexcept
{
    for (;;) {
        const auto seq1 = m_seq.load(std::memory_order_acquire);
        if (seq1 & 1) {
            continue; // writer mid-write; the writer never blocks, retry
        }
        Request request;
        request.kind = static_cast<Kind>(m_kind.load(std::memory_order_relaxed));
        request.epoch = m_epoch.load(std::memory_order_relaxed);
        request.primaryPipe = m_primaryPipe.load(std::memory_order_relaxed);
        request.fadingPipe = m_fadingPipe.load(std::memory_order_relaxed);
        request.fadeOut = std::chrono::milliseconds{m_fadeOutMs.load(std::memory_order_relaxed)};
        request.fadeIn = std::chrono::milliseconds{m_fadeInMs.load(std::memory_order_relaxed)};
        if (m_seq.load(std::memory_order_acquire) != seq1) {
            continue; // torn read; retry
        }
        return request;
    }
}

bool StreamSwitch::stage(const Request &request) noexcept
{
    if (m_pendingSeq.load(std::memory_order_acquire) != 0) {
        return false; // single outstanding request
    }
    writeRequest(request);
    m_pendingSeq.store(m_seq.load(std::memory_order_relaxed), std::memory_order_release);
    return true;
}

StreamSwitch::Request StreamSwitch::supersede(const Request &replacement) noexcept
{
    const Request previous = readRequest();
    writeRequest(replacement);
    m_pendingSeq.store(m_seq.load(std::memory_order_relaxed), std::memory_order_release);
    return previous;
}

StreamSwitch::Request StreamSwitch::clear() noexcept
{
    const Request previous = readRequest();
    writeRequest(Request{});
    m_pendingSeq.store(0, std::memory_order_release);
    return previous;
}

bool StreamSwitch::isPending() const noexcept
{
    return m_pendingSeq.load(std::memory_order_seq_cst) != 0;
}

StreamSwitch::Request StreamSwitch::peek() const noexcept
{
    return readRequest();
}

bool StreamSwitch::hasFadingPipe(const Pipe<float> *pipe) const noexcept
{
    return pipe != nullptr && m_pendingSeq.load(std::memory_order_acquire) != 0 && m_fadingPipe.load(std::memory_order_acquire) == pipe;
}

bool StreamSwitch::poll() const noexcept
{
    return m_pendingSeq.load(std::memory_order_acquire) != 0;
}

StreamSwitch::Request StreamSwitch::take() noexcept
{
    for (;;) {
        const auto pendingSeq = m_pendingSeq.load(std::memory_order_acquire);
        if (pendingSeq == 0) {
            return {};
        }
        if ((pendingSeq & 1) != 0) {
            // Still claimed by a previous take()
            return {};
        }
        const auto seq1 = m_seq.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0 || seq1 != pendingSeq) {
            continue; // writer mid-write, or the request was superseded since
        }
        const Request request = readRequest();
        auto expected = pendingSeq;
        if (m_pendingSeq.compare_exchange_strong(expected, pendingSeq | 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (m_seq.load(std::memory_order_acquire) != seq1) {
                // The writer replaced the request; the claim bit was
                // overwritten with the new sequence, so nothing to release.
                continue;
            }
            m_claimedSeq = seq1;
            return request;
        }
        // Superseded concurrently; retry to pick up the replacement
    }
}

void StreamSwitch::acknowledge() noexcept
{
    const auto claimedSeq = m_claimedSeq;
    if (claimedSeq == 0) {
        return;
    }
    m_claimedSeq = 0;
    auto expected = claimedSeq | 1;
    std::ignore = m_pendingSeq.compare_exchange_strong(expected, 0, std::memory_order_seq_cst, std::memory_order_acquire);
    if (m_ackedCallback) {
        m_ackedCallback();
    }
}
