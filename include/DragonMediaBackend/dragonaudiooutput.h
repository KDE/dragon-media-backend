/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>

#include <memory>

class DragonPlayer;
class DragonAudioSink;
class DragonAudioOutputPrivate;

class DRAGONMEDIABACKEND_EXPORT DragonAudioOutput : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(Backend backend READ backend CONSTANT)

public:
    enum class Backend {
        Auto,
        PipeWire,
        PulseAudio,
        SDL
    };
    Q_ENUM(Backend)

    [[nodiscard]] qreal volume() const;
    [[nodiscard]] bool muted() const;
    [[nodiscard]] Backend backend() const;

public Q_SLOTS:
    void setVolume(qreal linearGain);
    void setMuted(bool muted);

Q_SIGNALS:
    void volumeChanged();
    void mutedChanged(bool muted);

private:
    explicit DragonAudioOutput(Backend requested, DragonPlayer *parent);
    ~DragonAudioOutput() override;
    friend class DragonPlayer;
    friend class DragonPlayerPrivate;
    friend class DragonDiagnostics;

    DragonAudioSink *sink() const;

    std::unique_ptr<DragonAudioOutputPrivate> d;
};
