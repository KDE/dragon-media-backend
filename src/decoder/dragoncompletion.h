/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <optional>

#include <QMetaObject>
#include <QObject>
#include <QString>

#include "dragonmultimedia_export.h"

namespace DragonMultimedia
{

struct InitResult {
    bool success = false;
    bool cancelled = false;
    int sampleRate = 0;
    int channels = 0;
    int64_t durationMs = -1;
    bool isGapless = false;
    QString errorMessage;
};

// One-shot bridge that hands a result from a worker thread to a coroutine
// awaiting it on the Qt main thread.
//
// DragonDecodePipeline runs decoder initialization on a std::jthread, but
// DragonPlayer::setSource() is a QCoro coroutine that needs the InitResult
// (sample rate, channels, duration, success/error) before it can proceed.
// The pipeline creates a DragonCompletion, hands it to the worker, and
// `co_await`s it from the main thread. When the worker calls setResult()
// (or cancel()), the awaiting coroutine resumes with the InitResult.
//
// Backed by a Qt signal (finished) so Qt's AutoConnection handles the
// same-thread vs cross-thread dispatch and connection-lifetime safety.
// The result can be set before, during, or after the await; if it arrives
// before the await, it is buffered and the await resolves synchronously.
class DRAGONMULTIMEDIA_EXPORT DragonCompletion : public QObject
{
    Q_OBJECT
public:
    DragonCompletion();
    ~DragonCompletion() override;

    DragonCompletion(const DragonCompletion &) = delete;
    DragonCompletion &operator=(const DragonCompletion &) = delete;
    DragonCompletion(DragonCompletion &&) = delete;
    DragonCompletion &operator=(DragonCompletion &&) = delete;

    [[nodiscard]] bool isReady() const;

    // Synchronous access to the completed result. Only valid when isReady().
    [[nodiscard]] InitResult result() const;

    bool setResult(InitResult result);
    bool cancel(const QString &reason = {});

    // Awaitable interface. `co_await *completion` returns the InitResult.
    auto operator co_await()
    {
        struct State {
            QMetaObject::Connection conn;
            std::atomic<bool> resumed{false};
            std::coroutine_handle<> handle;
        };
        struct Awaiter {
            DragonCompletion *self;
            std::shared_ptr<State> state;

            [[nodiscard]] bool await_ready() const noexcept
            {
                return self->isReady();
            }

            void await_suspend(std::coroutine_handle<> h)
            {
                state->handle = h;
                // Receiver is `self` so the connection auto-disconnects when
                // the DragonCompletion is destroyed (Qt lifetime safety).
                state->conn = connect(
                    self,
                    &DragonCompletion::finished,
                    self,
                    [state = state](const InitResult &) {
                        if (!state->resumed.exchange(true, std::memory_order_acq_rel)) {
                            if (state->conn) {
                                disconnect(state->conn);
                                state->conn = {};
                            }
                            state->handle.resume();
                        }
                    },
                    Qt::AutoConnection);

                // Double-check: result may have arrived while we were connecting.
                if (self->isReady()) {
                    if (!state->resumed.exchange(true, std::memory_order_acq_rel)) {
                        if (state->conn) {
                            disconnect(state->conn);
                            state->conn = {};
                        }
                        state->handle.resume();
                    }
                }
            }

            [[nodiscard]] InitResult await_resume() const
            {
                return self->result();
            }
        };
        return Awaiter{this, std::make_shared<State>()};
    }

Q_SIGNALS:
    void finished(const InitResult &result);

private:
    std::atomic<bool> m_ready{false};
    std::optional<InitResult> m_buffered;
};

}

Q_DECLARE_METATYPE(DragonMultimedia::InitResult)
