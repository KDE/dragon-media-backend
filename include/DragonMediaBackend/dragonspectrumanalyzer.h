/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonfftframe.h"
#include "dragonmediabackend_export.h"

#include <QObject>

#include <cstddef>
#include <memory>

class DragonPlayer;
class DragonSpectrumAnalyzerPrivate;

/*!
 * \class DragonSpectrumAnalyzer
 * \inmodule DragonMediaBackend
 *
 * \brief Control surface for the FFT visualization stream.
 *
 * DragonSpectrumAnalyzer gives applications access to the spectrum data
 * computed by the visualization stage of a \l DragonPlayer pipeline.
 * While active, it emits frameReady() with \l DragonFftFrame values at
 * the configured frame rate, typically 60fps, which can drive bar and
 * spectrogram visualizers.
 *
 * The analyzer is inactive unless mode() selects a visualization mode,
 * so applications that do not visualize audio pay no FFT cost.
 *
 * \sa DragonPlayer, DragonFftFrame
 */
class DRAGONMEDIABACKEND_EXPORT DragonSpectrumAnalyzer : public QObject
{
    Q_OBJECT

    /*!
     * \property DragonSpectrumAnalyzer::mode
     *
     * Which parts of the frame data are computed and delivered. Setting
     * the mode to Off deactivates the analyzer.
     */
    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)

    /*!
     * \property DragonSpectrumAnalyzer::frameRate
     *
     * The target number of frames delivered per second. Clamped to the
     * range supported by the pipeline.
     */
    Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY frameRateChanged)

    /*!
     * \property DragonSpectrumAnalyzer::active
     *
     * Whether frames are currently being emitted. This is \c true when
     * the mode is not Off and the player pipeline is running.
     */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    /*!
     * \enum DragonSpectrumAnalyzer::Mode
     *
     * The visualization data to deliver.
     *
     * \value Off
     *        No visualization data is computed.
     * \value BarsOnly
     *        Only the smoothed bar magnitudes are delivered.
     * \value DetailedOnly
     *        Only the detailed frequency magnitudes are delivered.
     * \value Both
     *        Both bars and detailed magnitudes are delivered.
     */
    enum class Mode {
        Off,
        BarsOnly,
        DetailedOnly,
        Both
    };
    Q_ENUM(Mode)

    /*!
     * Constructs an analyzer for the pipeline of \a player, with \a parent
     * as its QObject parent. The analyzer starts in the Off mode.
     */
    explicit DragonSpectrumAnalyzer(DragonPlayer *player, QObject *parent = nullptr);
    ~DragonSpectrumAnalyzer() override;

    DragonSpectrumAnalyzer(const DragonSpectrumAnalyzer &) = delete;
    DragonSpectrumAnalyzer &operator=(const DragonSpectrumAnalyzer &) = delete;
    DragonSpectrumAnalyzer(DragonSpectrumAnalyzer &&) = delete;
    DragonSpectrumAnalyzer &operator=(DragonSpectrumAnalyzer &&) = delete;

    /*!
     * Returns the visualization mode.
     */
    [[nodiscard]] Mode mode() const;

    /*!
     * Returns the target frame rate in frames per second.
     */
    [[nodiscard]] int frameRate() const;

    /*!
     * Returns \c true if frames are currently being emitted.
     */
    [[nodiscard]] bool isActive() const;

public Q_SLOTS:
    /*!
     * Sets the visualization mode to \a mode. Setting Off stops frame
     * emission, any other value starts it.
     */
    void setMode(Mode mode);

    /*!
     * Sets the target frame rate to \a framesPerSecond.
     */
    void setFrameRate(int framesPerSecond);

Q_SIGNALS:
    /*!
     * Emitted when the visualization mode changes to \a mode.
     */
    void modeChanged(Mode mode);

    /*!
     * Emitted when the target frame rate changes to \a rate.
     */
    void frameRateChanged(int rate);

    /*!
     * Emitted when frame emission starts or stops, with the new state in
     * \a active.
     */
    void activeChanged(bool active);

    /*!
     * Emitted while the analyzer is active, delivering one \a frame of
     * spectrum data at the configured frame rate.
     */
    void frameReady(const DragonFftFrame &frame);

private:
    friend class DragonDiagnostics;

    [[nodiscard]] std::size_t fftPipeReady() const;

    std::unique_ptr<DragonSpectrumAnalyzerPrivate> d;
};
