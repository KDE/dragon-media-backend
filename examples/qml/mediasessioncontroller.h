/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <QObject>

class DragonPlayer;
class QTimer;

class DragonAndroidMediaSessionController : public QObject
{
    Q_OBJECT
public:
    explicit DragonAndroidMediaSessionController(DragonPlayer *player, QObject *parent = nullptr);
    ~DragonAndroidMediaSessionController() override;

private:
    void startMediaSessionService();
    void stopMediaSessionService();
    void pushSourceTitle();

    DragonPlayer *m_player = nullptr;
    QTimer *m_positionTimer = nullptr;
    bool m_serviceStarted = false;
    bool m_serviceStopPending = false;
};
