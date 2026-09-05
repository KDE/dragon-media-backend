/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragonradiostream.h>

#include <dragonbufferprogress.h>

#include "dragonmediabackend_network_logging.h"
#include <KLocalizedString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <cstring>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

DragonRadioStream::DragonRadioStream(QObject *parent)
    : DragonStream(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_bufferProgress(new DragonBufferProgress(this))
{
}

DragonRadioStream::~DragonRadioStream()
{
    stop();
}

void DragonRadioStream::setUrl(const QUrl &url)
{
    m_url = url;
}

void DragonRadioStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "start" << m_url.toString();

    m_abort = false;
    m_error = false;
    m_finished = false;
    m_icyMetaint = 0;
    m_icyBytesRead = 0;
    m_icyPendingData.clear();
    m_lastMetadata.clear();
    m_totalSize = -1;
    m_streamPosition = 0;
    if (m_acceptsRanges.exchange(false)) {
        Q_EMIT seekableChanged(false);
    }

    m_bufferProgress->reset();

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }

    QNetworkRequest request(m_url);
    request.setRawHeader("Icy-Metadata"_ba, "1"_ba);
    request.setTransferTimeout(std::chrono::seconds(5));
    m_reply = m_nam->get(request);
    m_reply->setReadBufferSize(MAX_BUFFER_BYTES);

    connect(m_reply, &QNetworkReply::encrypted, this, &DragonRadioStream::onReplyEncrypted);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &DragonRadioStream::onReplyMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &DragonRadioStream::onReplyReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DragonRadioStream::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &DragonRadioStream::onReplyError);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 bytesReceived, qint64) {
        m_bufferProgress->setBytesReceived(bytesReceived);
    });
}

void DragonRadioStream::stop()
{
    m_abort = true;

    m_bufferCv.notify_all();

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    {
        std::scoped_lock lock(m_bufferMutex);
        m_networkBuffer.clear();
        m_bufferOffset = 0;
    }
}

int DragonRadioStream::read(std::span<uint8_t> buf, std::stop_token st)
{
    std::unique_lock lock(m_bufferMutex);

    m_bufferCv.wait(lock, st, [this] {
        return !m_networkBuffer.empty() || m_abort.load() || m_error.load() || m_finished.load();
    });

    if (st.stop_requested() || m_abort || m_error) {
        return -1;
    }

    if (m_finished && m_networkBuffer.empty()) {
        return 0;
    }

    int bytesRead = 0;
    int toRead = static_cast<int>(buf.size());

    while (bytesRead < toRead && !m_networkBuffer.empty()) {
        const QByteArray &front = m_networkBuffer.front();
        int available = front.size() - m_bufferOffset;
        int take = std::min(toRead - bytesRead, available);

        std::memcpy(buf.data() + bytesRead, front.constData() + m_bufferOffset, take);
        bytesRead += take;
        m_bufferOffset += take;

        if (m_bufferOffset >= front.size()) {
            m_networkBuffer.pop_front();
            m_bufferOffset = 0;
        }
    }

    m_streamPosition += bytesRead;

    if (bytesRead > 0) {
        const auto prev = m_bufferDepth.fetch_sub(bytesRead);
        const auto newDepth = prev - bytesRead;
        if (prev > LOW_WATER_MARK && newDepth <= LOW_WATER_MARK) {
            m_isBuffering = true;
            Q_EMIT streamBuffering();
        }
        // If we were holding back due to a full buffer, there is space now.
        // Hop to the reply's thread to drain whatever accumulated.
        if (newDepth < MAX_BUFFER_BYTES && m_reply) {
            QMetaObject::invokeMethod(
                m_reply,
                [this]() {
                    drainReply();
                },
                Qt::QueuedConnection);
        }
    }

    return bytesRead;
}

qint64 DragonRadioStream::seek(qint64 offset)
{
    if (!m_acceptsRanges.load()) {
        return -1;
    }

    qCDebug(dragonMediaBackendNetwork) << "seek to byte offset" << offset;

    stop();

    m_abort = false;
    m_error = false;
    m_finished = false;
    m_bufferOffset = 0;
    m_streamPosition = offset;
    m_icyBytesRead = 0;
    m_icyPendingData.clear();

    m_bufferProgress->reset();

    QNetworkRequest request(m_url);
    request.setRawHeader("Icy-Metadata"_ba, "1"_ba);
    request.setRawHeader("Range"_ba, "bytes="_ba + QByteArray::number(offset) + "-"_ba);
    request.setTransferTimeout(std::chrono::seconds(5));
    m_reply = m_nam->get(request);
    m_reply->setReadBufferSize(MAX_BUFFER_BYTES);

    connect(m_reply, &QNetworkReply::encrypted, this, &DragonRadioStream::onReplyEncrypted);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &DragonRadioStream::onReplyMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &DragonRadioStream::onReplyReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DragonRadioStream::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &DragonRadioStream::onReplyError);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 bytesReceived, qint64) {
        m_bufferProgress->setBytesReceived(bytesReceived);
    });

    return offset;
}

qint64 DragonRadioStream::size() const
{
    return m_totalSize.load();
}

qint64 DragonRadioStream::position() const
{
    return m_streamPosition.load();
}

bool DragonRadioStream::isAborted() const
{
    return m_abort;
}

DragonBufferProgress *DragonRadioStream::bufferProgress() const
{
    return m_bufferProgress;
}

void DragonRadioStream::onReplyEncrypted()
{
    m_bufferProgress->setTlsHandshakeComplete(true);
}

void DragonRadioStream::onReplyMetaDataChanged()
{
    if (!m_bufferProgress->headersReceived()) {
        if (!m_bufferProgress->isTlsHandshakeComplete()) {
            m_bufferProgress->setTlsHandshakeComplete(true);
        }
        m_bufferProgress->setHeadersReceived(true);
    }

    if (m_reply) {
        const QByteArray metaintHeader = m_reply->rawHeader("icy-metaint"_ba);
        if (!metaintHeader.isEmpty()) {
            bool ok = false;
            const int metaint = metaintHeader.toInt(&ok);
            if (ok && metaint > 0) {
                m_icyMetaint = metaint;
                qCDebug(dragonMediaBackendNetwork) << "ICY metadata interval:" << m_icyMetaint;
            }
        }

        const QByteArray contentLength = m_reply->rawHeader("Content-Length"_ba);
        if (!contentLength.isEmpty()) {
            bool ok = false;
            const qint64 cl = contentLength.toLongLong(&ok);
            if (ok && cl > 0) {
                const QByteArray contentRange = m_reply->rawHeader("Content-Range"_ba);
                if (!contentRange.isEmpty()) {
                    const int slashIdx = contentRange.lastIndexOf('/');
                    if (slashIdx >= 0) {
                        bool okTotal = false;
                        const qint64 total = contentRange.mid(slashIdx + 1).toLongLong(&okTotal);
                        if (okTotal && total > 0) {
                            m_totalSize = total;
                            qCDebug(dragonMediaBackendNetwork) << "Total size from Content-Range:" << total;
                        }
                    }
                } else {
                    m_totalSize = cl;
                    qCDebug(dragonMediaBackendNetwork) << "Total size from Content-Length:" << cl;
                }
            }
        }

        const QByteArray acceptRanges = m_reply->rawHeader("Accept-Ranges"_ba);
        if (acceptRanges.toLower() == "bytes") {
            if (!m_acceptsRanges.exchange(true)) {
                qCDebug(dragonMediaBackendNetwork) << "Server supports Range requests";
                Q_EMIT seekableChanged(true);
            }
        }
    }
}

void DragonRadioStream::onReplyReadyRead()
{
    // Runs on the reply's thread. If the buffer is already full, leave the
    // data in the reply's internal buffer; setReadBufferSize() throttles the
    // socket so memory cannot grow without bound. read() re-invokes
    // drainReply() once the consumer frees space.
    drainReply();
}

void DragonRadioStream::drainReply()
{
    if (!m_reply) {
        return;
    }

    // When the reply has finished we must drain its tail regardless of the
    // cap, otherwise data held back by backpressure would be lost.
    const bool finished = m_reply->isFinished();
    if (!finished && m_bufferDepth.load() >= MAX_BUFFER_BYTES) {
        return;
    }

    const QByteArray data = m_reply->readAll();
    if (data.isEmpty()) {
        return;
    }

    if (m_icyMetaint > 0) {
        processIcyData(data);
    } else {
        addToAudioBuffer(data);
    }
}

void DragonRadioStream::onReplyFinished()
{
    qCDebug(dragonMediaBackendNetwork) << "reply finished";

    if (m_abort) {
        return;
    }

    if (m_reply && m_reply->error() != QNetworkReply::NoError) {
        qCDebug(dragonMediaBackendNetwork) << "reply finished with error, will reconnect";
        QTimer::singleShot(2000, this, [this]() {
            if (!m_abort) {
                start();
            }
        });
    } else {
        qCDebug(dragonMediaBackendNetwork) << "reply finished successfully, no reconnect needed";
        // Drain anything the backpressure guard held back before EOF.
        drainReply();
        m_finished = true;
        m_bufferCv.notify_all();
    }
}

void DragonRadioStream::onReplyError(QNetworkReply::NetworkError code)
{
    qCWarning(dragonMediaBackendNetwork) << "error:" << code;
    m_error = true;
    m_bufferCv.notify_all();

    if (code == QNetworkReply::TimeoutError) {
        Q_EMIT streamStalled();
    }

    if (!m_abort) {
        Q_EMIT errorOccurred(i18n("Network error: %1", static_cast<int>(code)));
    }
}

void DragonRadioStream::addToAudioBuffer(const QByteArray &data)
{
    {
        std::scoped_lock lock(m_bufferMutex);
        m_networkBuffer.push_back(data);
    }
    const qint64 newDepth = m_bufferDepth += data.size();
    m_bufferCv.notify_all();

    if (m_isBuffering.load() && newDepth >= HIGH_WATER_MARK) {
        m_isBuffering = false;
        Q_EMIT streamBuffered();
    }
}

void DragonRadioStream::processIcyData(const QByteArray &data)
{
    QByteArray buffer = m_icyPendingData;
    buffer.append(data);
    m_icyPendingData.clear();

    int pos = 0;
    while (pos < buffer.size()) {
        const int bytesUntilMeta = m_icyMetaint - m_icyBytesRead;

        if (bytesUntilMeta > 0) {
            const int chunkSize = std::min(static_cast<int>(buffer.size() - pos), bytesUntilMeta);
            if (chunkSize > 0) {
                addToAudioBuffer(QByteArrayView(buffer).sliced(pos, chunkSize).toByteArray());
                pos += chunkSize;
                m_icyBytesRead += chunkSize;
            }
        } else {
            if (pos >= buffer.size()) {
                break;
            }

            const uint8_t lengthByte = static_cast<uint8_t>(buffer[pos++]);

            if (lengthByte == 0) {
                m_icyBytesRead = 0;
                continue;
            }

            const int metadataLength = lengthByte * 16;
            if (pos + metadataLength > buffer.size()) {
                --pos;
                break;
            }

            const QByteArray meta = QByteArrayView(buffer).sliced(pos, metadataLength).toByteArray();
            processMetadata(meta);

            pos += metadataLength;
            m_icyBytesRead = 0;
        }
    }

    if (pos < buffer.size()) {
        m_icyPendingData = buffer.mid(pos);
    }
}

void DragonRadioStream::processMetadata(const QByteArray &metadata)
{
    static constexpr auto wsOrNul = [](const char c) {
        return c == ' ' || c == '\0';
    };

    auto ltrim = [](const QByteArrayView v) {
        qsizetype i = 0;
        while (i < v.size() && wsOrNul(v[i]))
            ++i;
        return v.sliced(i);
    };

    DragonIcyMetadata icy;
    QByteArrayView v(metadata);

    while (!(v = ltrim(v)).isEmpty()) {
        const qsizetype eq = v.indexOf('=');
        if (eq < 0)
            break;
        QString key = QString::fromUtf8(v.first(eq)).trimmed();
        v = v.sliced(eq + 1);
        if (v.isEmpty() || v.front() != '\'')
            break;
        v = v.sliced(1);

        QByteArray val;
        while (!v.isEmpty()) {
            if (const qsizetype q = v.indexOf('\''); q >= 0) {
                val.append(v.first(q).toByteArray());
                v = v.sliced(q + 1);
                if (!v.isEmpty() && v.front() == '\'') {
                    val.append('\'');
                    v = v.sliced(1);
                } else
                    break;
            } else {
                val.append(v.toByteArray());
                v = {};
                break;
            }
        }

        if (!key.isEmpty()) {
            const QString value = QString::fromUtf8(val).trimmed();
            if (key == "StreamTitle"_L1) {
                icy.setStreamTitle(value);
            } else if (key == "StreamUrl"_L1) {
                icy.setStreamUrl(value);
            } else {
                icy.insertCustomField(key, value);
            }
        }
        if (!v.isEmpty() && v.front() == ';')
            v = v.sliced(1);
    }

    if (icy != m_lastMetadata) {
        m_lastMetadata = icy;
        Q_EMIT metadataReady(icy);
    }
}
