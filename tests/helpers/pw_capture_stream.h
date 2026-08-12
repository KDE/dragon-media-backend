/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Capture stream that connects to a sink's monitor ports and accumulates
 * rendered PCM data for offline analysis.
 *
 * Uses PW_KEY_STREAM_CAPTURE_SINK + PW_KEY_TARGET_OBJECT for auto-linking,
 * which requires a session manager (WirePlumber) to be running.
 * Runs its own pw_main_loop on a background thread.
 */

#pragma once

#include <QDebug>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class PwCaptureStream
{
public:
    PwCaptureStream() = default;

    ~PwCaptureStream()
    {
        stop();
    }

    PwCaptureStream(const PwCaptureStream &) = delete;
    PwCaptureStream &operator=(const PwCaptureStream &) = delete;

    bool connect(const QString &targetNode, int sampleRate, int channels)
    {
        pw_init(nullptr, nullptr);

        m_channels = channels;
        m_sampleRate = sampleRate;
        m_targetNode = targetNode.toStdString();
        m_connected.store(false, std::memory_order_release);
        m_error.store(false, std::memory_order_release);
        m_receivingData.store(false, std::memory_order_release);

        m_thread = std::jthread([this](std::stop_token st) {
            runCaptureLoop(st);
        });

        for (int i = 0; i < 200; ++i) {
            if (m_connected.load(std::memory_order_acquire)) {
                return true;
            }
            if (m_error.load(std::memory_order_acquire)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return m_connected.load();
    }

    bool waitForData(int timeoutMs = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_receivingData.load(std::memory_order_acquire)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    void stop()
    {
        if (m_stopped.exchange(true)) {
            return;
        }
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        pw_deinit();
    }

    std::vector<float> takePcm()
    {
        std::lock_guard lock(m_mutex);
        return std::move(m_pcm);
    }

    qint64 totalFrames() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<qint64>(m_pcm.size()) / m_channels;
    }

    int channels() const
    {
        return m_channels;
    }

private:
    void runCaptureLoop(std::stop_token st)
    {
        pw_main_loop *loop = pw_main_loop_new(nullptr);
        if (!loop) {
            m_error.store(true);
            return;
        }

        pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
        if (!context) {
            pw_main_loop_destroy(loop);
            m_error.store(true);
            return;
        }

        pw_core *core = pw_context_connect(context, nullptr, 0);
        if (!core) {
            qWarning() << "PwCaptureStream: failed to connect to PipeWire";
            pw_context_destroy(context);
            pw_main_loop_destroy(loop);
            m_error.store(true);
            return;
        }

        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                 "Audio",
                                                 PW_KEY_MEDIA_CATEGORY,
                                                 "Capture",
                                                 PW_KEY_STREAM_CAPTURE_SINK,
                                                 "true",
                                                 PW_KEY_TARGET_OBJECT,
                                                 m_targetNode.c_str(),
                                                 nullptr);

        m_stream = pw_stream_new(core, "dragon-gapless-capture", props);
        if (!m_stream) {
            qWarning() << "PwCaptureStream: failed to create stream";
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_main_loop_destroy(loop);
            m_error.store(true);
            return;
        }

        pw_stream_events streamEvents{};
        streamEvents.version = PW_VERSION_STREAM_EVENTS;
        streamEvents.process = [](void *ud) {
            static_cast<PwCaptureStream *>(ud)->onProcess();
        };

        spa_hook streamListener{};
        pw_stream_add_listener(m_stream, &streamListener, &streamEvents, this);

        uint8_t podBuffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

        struct spa_audio_info_raw audioInfo = {};
        audioInfo.format = SPA_AUDIO_FORMAT_F32;
        audioInfo.rate = static_cast<uint32_t>(m_sampleRate);
        audioInfo.channels = static_cast<uint32_t>(m_channels);

        const struct spa_pod *params = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audioInfo);
        if (!params) {
            qWarning() << "PwCaptureStream: failed to build format pod";
            m_error.store(true);
            return;
        }

        constexpr auto streamFlags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);

        int res = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, streamFlags, &params, 1);
        if (res != 0) {
            qWarning() << "PwCaptureStream: failed to connect stream:" << res;
            m_error.store(true);
            return;
        }

        qDebug() << "PwCaptureStream: connected to" << QString::fromStdString(m_targetNode);
        m_connected.store(true, std::memory_order_release);

        pw_loop *loopImpl = pw_main_loop_get_loop(loop);
        while (!st.stop_requested()) {
            pw_loop_iterate(loopImpl, 50);
        }

        spa_hook_remove(&streamListener);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
    }

    void onProcess()
    {
        if (!m_stream) {
            return;
        }

        struct pw_buffer *pwBuf = pw_stream_dequeue_buffer(m_stream);
        if (!pwBuf) {
            return;
        }

        struct spa_buffer *spaBuf = pwBuf->buffer;
        if (!spaBuf || spaBuf->n_datas < 1 || !spaBuf->datas[0].data) {
            pw_stream_queue_buffer(m_stream, pwBuf);
            return;
        }

        uint32_t size = spaBuf->datas[0].chunk->size;
        if (size == 0) {
            pw_stream_queue_buffer(m_stream, pwBuf);
            return;
        }

        const float *src = static_cast<const float *>(spaBuf->datas[0].data);
        int numFloats = static_cast<int>(size / sizeof(float));

        {
            std::lock_guard lock(m_mutex);
            m_pcm.insert(m_pcm.end(), src, src + numFloats);
        }

        m_receivingData.store(true, std::memory_order_release);
        pw_stream_queue_buffer(m_stream, pwBuf);
    }

    mutable std::mutex m_mutex;
    std::vector<float> m_pcm;
    int m_channels = 0;
    int m_sampleRate = 0;
    std::string m_targetNode;

    std::jthread m_thread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_error{false};
    std::atomic<bool> m_receivingData{false};
    std::atomic<bool> m_stopped{false};

    pw_stream *m_stream = nullptr;
};
