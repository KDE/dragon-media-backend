/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonfftframe.h"
#include "dragonmultimedia_export.h"

#include <QObject>

#include <memory>

class DragonPlayer;
class DragonSpectrumAnalyzerPrivate;

class DRAGONMULTIMEDIA_EXPORT DragonSpectrumAnalyzer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY frameRateChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    enum class Mode {
        Off,
        BarsOnly,
        DetailedOnly,
        Both
    };
    Q_ENUM(Mode)

    explicit DragonSpectrumAnalyzer(DragonPlayer *player, QObject *parent = nullptr);
    ~DragonSpectrumAnalyzer() override;

    DragonSpectrumAnalyzer(const DragonSpectrumAnalyzer &) = delete;
    DragonSpectrumAnalyzer &operator=(const DragonSpectrumAnalyzer &) = delete;
    DragonSpectrumAnalyzer(DragonSpectrumAnalyzer &&) = delete;
    DragonSpectrumAnalyzer &operator=(DragonSpectrumAnalyzer &&) = delete;

    [[nodiscard]] DragonPlayer *player() const;
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] int frameRate() const;
    [[nodiscard]] bool isActive() const;

public Q_SLOTS:
    void setMode(Mode mode);
    void setFrameRate(int framesPerSecond);

Q_SIGNALS:
    void modeChanged(Mode mode);
    void frameRateChanged(int rate);
    void activeChanged(bool active);
    void frameReady(const DragonFftFrame &frame);

private:
    std::unique_ptr<DragonSpectrumAnalyzerPrivate> d;
};
