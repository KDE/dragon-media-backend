/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QMetaType>
#include <QSharedDataPointer>

#include <chrono>
#include <cstddef>
#include <span>

class DragonFftFramePrivate;

/*!
 * \class DragonFftFrame
 * \inmodule DragonMediaBackend
 * \inheaderfile DragonFftFrame
 *
 * \brief Value object holding one FFT visualization frame.
 *
 * A DragonFftFrame carries the frequency magnitudes computed by the
 * visualization pipeline for a single point in time: 512
 * \l {https://en.wikipedia.org/wiki/Mel_scale}{mel scale} frequency magnitudes in dB, 24
 * smoothed bar magnitudes derived from them, and the timestamp of the audio they were computed
 * from.
 *
 * Instances are emitted by \l DragonSpectrumAnalyzer through its
 * frameReady() signal at roughly 60fps while visualization is active.
 *
 * The frame is implicitly shared, so copying one is cheap.
 *
 * \sa DragonSpectrumAnalyzer
 */
class DRAGONMEDIABACKEND_EXPORT DragonFftFrame
{
public:
    /*!
     * The number of mel scale frequency magnitudes in frequencies().
     */
    static constexpr std::size_t NUM_FREQUENCIES = 512;

    /*!
     * The number of smoothed bar magnitudes in bars().
     */
    static constexpr std::size_t NUM_BARS = 24;

    /*!
     * Constructs a null frame. frequencies(), bars() and timestamp()
     * return empty and default values respectively.
     */
    DragonFftFrame();

    /*!
     * Constructs a frame from \a frequenciesDb (magnitude in dB per
     * mel scale bin), \a barData (smoothed bar magnitudes) and the
     * audio position \a timestamp the data was computed from.
     */
    DragonFftFrame(std::span<const float> frequenciesDb, std::span<const float> barData, std::chrono::microseconds timestamp);

    /*!
     * Copies \a other. The underlying data is implicitly shared.
     */
    DragonFftFrame(const DragonFftFrame &other);

    /*!
     * Assigns \a other to this frame. The underlying data is implicitly
     * shared.
     */
    DragonFftFrame &operator=(const DragonFftFrame &other);

    /*!
     * Moves \a other into this frame, leaving \a other null.
     */
    DragonFftFrame(DragonFftFrame &&other) noexcept;

    /*!
     * Move-assigns \a other into this frame, leaving \a other null.
     */
    DragonFftFrame &operator=(DragonFftFrame &&other) noexcept;

    ~DragonFftFrame();

    /*!
     * Returns the NUM_FREQUENCIES mel scale frequency magnitudes in dB.
     *
     * The returned span is empty for a null frame.
     */
    [[nodiscard]] std::span<const float> frequencies() const;

    /*!
     * Returns the NUM_BARS smoothed bar magnitudes.
     *
     * The returned span is empty for a null frame.
     */
    [[nodiscard]] std::span<const float> bars() const;

    /*!
     * Returns the position in the audio stream the frame was computed
     * from.
     */
    [[nodiscard]] std::chrono::microseconds timestamp() const;

    /*!
     * Returns \c true if \a other describes the same magnitudes and
     * timestamp as this frame.
     */
    [[nodiscard]] bool operator==(const DragonFftFrame &other) const;

    /*!
     * Returns \c true if \a other describes a different frame.
     */
    [[nodiscard]] bool operator!=(const DragonFftFrame &other) const;

private:
    QSharedDataPointer<DragonFftFramePrivate> d;
};

Q_DECLARE_METATYPE(DragonFftFrame)
