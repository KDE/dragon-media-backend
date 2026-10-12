/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragoncdtocreader.h>

#ifdef DRAGON_HAVE_LIBCDIO
#include "dragoncddisc.h"
#endif

#include <KLocalizedString>

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <utility>

class DragonCdTocReaderPrivate
{
public:
    explicit DragonCdTocReaderPrivate(DragonCdTocReader *reader)
        : q(reader)
    {
    }

    void requestToc(const QString &device);
    void workerDone(const DragonCdToc &toc, const QString &errorMessage);

    DragonCdTocReader *q = nullptr;

    bool running = false;
    QString runningDevice;
    QString pendingDevice;
};

class DragonCdTocAcquireJob : public QRunnable
{
public:
    DragonCdTocAcquireJob(DragonCdTocReader *reader, QString device)
        : m_reader(reader)
        , m_device(std::move(device))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        DragonCdToc toc;
        QString errorMessage;

#ifdef DRAGON_HAVE_LIBCDIO
        auto disc = DragonCdDisc::acquire(m_device);
        if (disc) {
            toc = disc.value()->toc();
        } else {
            errorMessage = disc.error();
        }
#else
        errorMessage = i18n("This build of Dragon Media Backend was made without CD support.");
#endif

        QMetaObject::invokeMethod(
            m_reader.data(),
            [reader = m_reader, toc = std::move(toc), errorMessage = std::move(errorMessage)]() mutable {
                if (reader) {
                    reader->d->workerDone(toc, errorMessage);
                }
            },
            Qt::QueuedConnection);
    }

private:
    QPointer<DragonCdTocReader> m_reader;
    QString m_device;
};

void DragonCdTocReaderPrivate::requestToc(const QString &device)
{
    if (running) {
        // Coalesce concurrent requests for the same device into the
        // in-flight read; a different device is read once this one ends.
        if (device != runningDevice) {
            pendingDevice = device;
        }
        return;
    }

    running = true;
    runningDevice = device;
    QThreadPool::globalInstance()->start(new DragonCdTocAcquireJob(q, device));
}

void DragonCdTocReaderPrivate::workerDone(const DragonCdToc &toc, const QString &errorMessage)
{
    running = false;
    runningDevice.clear();

    if (!errorMessage.isEmpty()) {
        Q_EMIT q->failed(errorMessage);
    } else {
        Q_EMIT q->tocReady(toc);
    }

    if (!pendingDevice.isEmpty()) {
        const QString next = pendingDevice;
        pendingDevice.clear();
        requestToc(next);
    }
}

DragonCdTocReader::DragonCdTocReader(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<DragonCdTocReaderPrivate>(this))
{
    // Cross-thread queued connections of tocReady() need the metatypes
    // registered once per process, not once per reader instance.
    [[maybe_unused]] static const bool once = [] {
        qRegisterMetaType<DragonCdToc>();
        qRegisterMetaType<DragonCdTrack>();
        return true;
    }();
}

DragonCdTocReader::~DragonCdTocReader() = default;

void DragonCdTocReader::requestToc(const QString &device)
{
    d->requestToc(device);
}
