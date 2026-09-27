/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class DragonPlayer;
class DragonAudioSink;
class DragonAudioOutputPrivate;

/*!
 * \class DragonAudioOutput
 * \inmodule DragonMediaBackend
 *
 * \brief Audio output controls, wrapping the active audio backend.
 *
 * DragonAudioOutput exposes volume and mute controls for the audio
 * pipeline, together with the backend that was selected at runtime. It
 * is created by \l DragonPlayer and accessed through
 * \l DragonPlayer::audioOutput.
 *
 * volume() takes a linear gain in the range 0.0 to 1.0. Applications
 * using a logarithmic volume scale should convert before setting it,
 * as \l DragonPlayer does for its QMediaPlayer-compatible API.
 *
 * \sa DragonPlayer::audioOutput
 */

/*!
 * \qmltype DragonAudioOutput
 * \nativetype DragonAudioOutput
 * \inqmlmodule org.kde.dragonmediabackend
 *
 * \brief Audio output controls for a DragonPlayer.
 *
 * This type cannot be instantiated from QML. Instances are created
 * and owned by DragonPlayer, and accessed through its audioOutput
 * property:
 *
 * \qml
 * DragonPlayer {
 *     id: player
 *     Component.onCompleted: player.audioOutput.volume = 0.8
 * }
 * \endqml
 *
 * The \l volume property is a linear gain from 0.0 to 1.0.
 * Applications using a logarithmic volume scale should convert before
 * setting it.
 *
 * \sa DragonPlayer::audioOutput
 */

class DRAGONMEDIABACKEND_EXPORT DragonAudioOutput : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(DragonAudioOutput)
    QML_UNCREATABLE("Instances are created and owned by DragonPlayer")

    /*!
     * \qmlproperty real org.kde.dragonmediabackend::DragonAudioOutput::volume
     *
     * The linear output gain, from 0.0 (silent) to 1.0 (full volume).
     */

    /*!
     * \property DragonAudioOutput::volume
     *
     * The linear output gain, from 0.0 (silent) to 1.0 (full volume).
     */
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)

    /*!
     * \qmlproperty bool org.kde.dragonmediabackend::DragonAudioOutput::muted
     *
     * Whether audio output is muted. Muting is independent of volume().
     */

    /*!
     * \property DragonAudioOutput::muted
     *
     * Whether audio output is muted. Muting is independent of volume().
     */
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)

    /*!
     * \qmlproperty enumeration org.kde.dragonmediabackend::DragonAudioOutput::backend
     *
     * The audio backend in use: DragonAudioOutput.PipeWire,
     * DragonAudioOutput.PulseAudio, DragonAudioOutput.SDL, or
     * DragonAudioOutput.Null. It is constant for the lifetime of the
     * player.
     */

    /*!
     * \property DragonAudioOutput::backend
     *
     * The audio backend this output uses. This property is constant.
     */
    Q_PROPERTY(Backend backend READ backend CONSTANT)

public:
    /*!
     * \enum DragonAudioOutput::Backend
     *
     * The audio backend used for output.
     *
     * \value Auto
     *        Probe the available backends and select the best one.
     * \value PipeWire
     *        Output through a PipeWire stream.
     * \value PulseAudio
     *        Output through PulseAudio.
     * \value SDL
     *        Output through an SDL3 audio device.
     * \value Null
     *        A null audio sink that produces no sound (fallback).
     */
    enum class Backend {
        Auto,
        PipeWire,
        PulseAudio,
        SDL,
        Null,
    };
    Q_ENUM(Backend)

    /*!
     * Returns the linear output gain, from 0.0 to 1.0.
     */
    [[nodiscard]] qreal volume() const;

    /*!
     * Returns \c true if audio output is muted.
     */
    [[nodiscard]] bool muted() const;

    /*!
     * Returns the audio backend in use. If Auto was requested, this is
     * the backend that was selected after probing.
     */
    [[nodiscard]] Backend backend() const;

public Q_SLOTS:
    /*!
     * Sets the output gain to \a linearGain, in the range 0.0 to 1.0.
     */
    void setVolume(qreal linearGain);

    /*!
     * Mutes the output if \a muted is \c true, otherwise unmutes it.
     */
    void setMuted(bool muted);

Q_SIGNALS:
    /*!
     * Emitted when the output volume changes.
     */
    void volumeChanged();

    /*!
     * Emitted when the output is muted or unmuted, with the new state in
     * \a muted.
     */
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
