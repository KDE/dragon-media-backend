/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragonradiostream.h>

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <cstring>

using namespace Qt::StringLiterals;

DragonRadioStream::DragonRadioStream(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_watchdogTimer(new QTimer(this))
{
    m_watchdogTimer->setInterval(5000);
    m_watchdogTimer->setSingleShot(true);
    connect(m_watchdogTimer, &QTimer::timeout, this, &DragonRadioStream::onWatchdogTimeout);
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
    qDebug() << "DragonRadioStream::start" << m_url.toString();

    m_abort = false;
    m_error = false;
    m_icyMetaint = 0;
    m_icyBytesRead = 0;
    m_icyPendingData.clear();
    m_lastMetadata.clear();

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }

    QNetworkRequest request(m_url);
    request.setRawHeader("Icy-Metadata", "1");
    m_reply = m_nam->get(request);

    connect(m_reply, &QNetworkReply::encrypted, this, &DragonRadioStream::onReplyEncrypted);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &DragonRadioStream::onReplyMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &DragonRadioStream::onReplyReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DragonRadioStream::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &DragonRadioStream::onReplyError);

    m_watchdogTimer->start();
}

void DragonRadioStream::stop()
{
    m_abort = true;

    if (m_watchdogTimer) {
        m_watchdogTimer->stop();
    }

    m_bufferCv.notify_all();

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    {
        std::lock_guard lock(m_bufferMutex);
        m_networkBuffer.clear();
        m_bufferOffset = 0;
    }
}

int DragonRadioStream::read(std::span<uint8_t> buf, std::stop_token st)
{
    std::unique_lock lock(m_bufferMutex);

    m_bufferCv.wait(lock, st, [this] {
        return !m_networkBuffer.empty() || m_abort.load() || m_error.load();
    });

    if (st.stop_requested() || m_abort || m_error) {
        return -1;
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

    return bytesRead;
}

bool DragonRadioStream::isAborted() const
{
    return m_abort;
}

void DragonRadioStream::onReplyEncrypted()
{
}

void DragonRadioStream::onReplyMetaDataChanged()
{
    if (m_reply) {
        const QByteArray metaintHeader = m_reply->rawHeader("icy-metaint");
        if (!metaintHeader.isEmpty()) {
            bool ok = false;
            const int metaint = metaintHeader.toInt(&ok);
            if (ok && metaint > 0) {
                m_icyMetaint = metaint;
                qDebug() << "ICY metadata interval:" << m_icyMetaint;
            }
        }
    }
}

void DragonRadioStream::onReplyReadyRead()
{
    if (!m_reply) {
        return;
    }
    m_watchdogTimer->start();

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
    qDebug() << "DragonRadioStream reply finished";
    m_watchdogTimer->stop();

    if (m_abort) {
        return;
    }

    QTimer::singleShot(2000, this, [this]() {
        if (!m_abort) {
            start();
        }
    });
}

void DragonRadioStream::onReplyError(QNetworkReply::NetworkError code)
{
    qWarning() << "DragonRadioStream error:" << code;
    m_watchdogTimer->stop();
    m_error = true;
    m_bufferCv.notify_all();

    if (!m_abort) {
        emit errorOccurred(u"Network error: %1"_s.arg(static_cast<int>(code)));
    }
}

void DragonRadioStream::onWatchdogTimeout()
{
    if (m_abort) {
        return;
    }

    qDebug() << "DragonRadioStream watchdog timeout reconnecting";
    start();
}

void DragonRadioStream::addToAudioBuffer(const QByteArray &data)
{
    {
        std::lock_guard lock(m_bufferMutex);
        m_networkBuffer.push_back(data);
    }
    m_bufferCv.notify_all();
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
            if (key == QStringLiteral("StreamTitle")) {
                icy.setStreamTitle(value);
            } else if (key == QStringLiteral("StreamUrl")) {
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
        emit metadataReady(icy);
    }
}
