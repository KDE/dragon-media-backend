/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests that the org.kde.dragonmediabackend QML module loads the way a
 * standalone engine process (qml6) loads it: via an import path, with no
 * context properties and no imperative registration.
 */

#include <QtTest>

#include "logging_timestamp_init.h"

#include <DragonPlayer>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <memory>

class TestQmlModule : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void moduleCreatesPlayer();
    void playerSurfaceVisibleInQml();

private:
    [[nodiscard]] static std::unique_ptr<QObject> createObject(QQmlEngine &engine, const QByteArray &qml);
};

std::unique_ptr<QObject> TestQmlModule::createObject(QQmlEngine &engine, const QByteArray &qml)
{
    engine.addImportPath(QStringLiteral(DRAGON_QML_IMPORT_PATH));
    QQmlComponent component(&engine);
    component.setData(qml, QUrl());
    if (component.status() != QQmlComponent::Ready) {
        qWarning() << component.errorString();
        return nullptr;
    }
    return std::unique_ptr<QObject>(component.create());
}

void TestQmlModule::moduleCreatesPlayer()
{
    QQmlEngine engine;
    const auto object = createObject(engine, QByteArrayLiteral(R"QML(
        import QtQml
        import org.kde.dragonmediabackend

        DragonPlayer { }
    )QML"));
    QVERIFY(object);

    auto *player = qobject_cast<DragonPlayer *>(object.get());
    QVERIFY(player);
    QCOMPARE(player->playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(player->audioOutput() != nullptr);
}

void TestQmlModule::playerSurfaceVisibleInQml()
{
    QQmlEngine engine;
    const auto object = createObject(engine, QByteArrayLiteral(R"QML(
        import QtQml
        import org.kde.dragonmediabackend

        DragonPlayer {
            id: player
            property var positionMs: player.position.milliseconds
            property var durationValid: player.duration.valid
            property real outputVolume: player.audioOutput.volume
            property var state: DragonPlayer.StoppedState
        }
    )QML"));
    QVERIFY(object);

    QCOMPARE(object->property("positionMs").toLongLong(), qint64(0));
    QCOMPARE(object->property("durationValid").toBool(), false);
    QCOMPARE(object->property("outputVolume").toReal(), 1.0);
    QCOMPARE(object->property("state").toInt(), static_cast<int>(DragonPlayer::PlaybackState::StoppedState));
}

QTEST_GUILESS_MAIN(TestQmlModule)
#include "test_qml_module.moc"
