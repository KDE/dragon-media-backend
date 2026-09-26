/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * QML integration tests for DragonDuration as the DragonPlayer property
 * serialization type: sub-property reads, invokables, and grouped-property
 * write-back through the chrono-based setters.
 */

#include <QtTest>

#include "logging_timestamp_init.h"

#include <DragonMediaBackend/dragonplayer.h>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QVariant>

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

class TestDragonDurationQml : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSubPropertyReads();
    void testFormattedInvokable();
    void testDurationInvalidForUnknownDuration();
    void testWriteBackThroughSubProperty();

private:
    [[nodiscard]] static std::unique_ptr<QObject> createObject(QQmlEngine &engine, DragonPlayer &player, const QByteArray &qml);
};

std::unique_ptr<QObject> TestDragonDurationQml::createObject(QQmlEngine &engine, DragonPlayer &player, const QByteArray &qml)
{
    engine.rootContext()->setContextProperty(QStringLiteral("player"), &player);
    QQmlComponent component(&engine);
    component.setData(qml, QUrl());
    if (component.status() != QQmlComponent::Ready) {
        qWarning() << component.errorString();
        return nullptr;
    }
    return std::unique_ptr<QObject>(component.create());
}

void TestDragonDurationQml::testSubPropertyReads()
{
    DragonPlayer player;
    QQmlEngine engine;
    const auto object = createObject(engine, player, QByteArrayLiteral(R"QML(
        import QtQml
        QtObject {
            property var ms: player.position.milliseconds
            property var sec: player.position.seconds
        }
    )QML"));
    QVERIFY(object);

    const QVariant ms = object->property("ms");
    QVERIFY(ms.isValid());
    QCOMPARE(ms.toDouble(), 0.0);

    const QVariant seconds = object->property("sec");
    QVERIFY(seconds.isValid());
    QCOMPARE(seconds.toDouble(), 0.0);
}

void TestDragonDurationQml::testFormattedInvokable()
{
    DragonPlayer player;
    player.setPrefinishMark(221'000ms);
    QQmlEngine engine;
    const auto object = createObject(engine, player, QByteArrayLiteral(R"QML(
        import QtQml
        QtObject {
            property var mark: player.prefinishMark.formatted()
            property var duration: player.duration.formatted()
        }
    )QML"));
    QVERIFY(object);

    QCOMPARE(object->property("mark").toString(), QStringLiteral("3:41"));
    QCOMPARE(object->property("duration").toString(), QStringLiteral("--:--"));
}

void TestDragonDurationQml::testDurationInvalidForUnknownDuration()
{
    DragonPlayer player;
    QQmlEngine engine;
    const auto object = createObject(engine, player, QByteArrayLiteral(R"QML(
        import QtQml
        QtObject {
            property var valid: player.duration.valid
            property var ms: player.duration.milliseconds
        }
    )QML"));
    QVERIFY(object);

    QCOMPARE(object->property("valid").toBool(), false);
    QCOMPARE(object->property("ms").toDouble(), 0.0);
}

void TestDragonDurationQml::testWriteBackThroughSubProperty()
{
    DragonPlayer player;
    QQmlEngine engine;
    const auto object = createObject(engine, player, QByteArrayLiteral(R"QML(
        import QtQml
        QtObject {
            function pokeMilliseconds(v) { player.prefinishMark.milliseconds = v; }
            function pokeSeconds(v) { player.prefinishMark.seconds = v; }
            function readMilliseconds() { return player.prefinishMark.milliseconds; }
        }
    )QML"));
    QVERIFY(object);

    QVERIFY(QMetaObject::invokeMethod(object.get(), "pokeMilliseconds", Q_ARG(QVariant, QVariant::fromValue(1234))));
    QCOMPARE(player.prefinishMark(), 1234ms);

    QVERIFY(QMetaObject::invokeMethod(object.get(), "pokeSeconds", Q_ARG(QVariant, QVariant::fromValue(1.5))));
    QCOMPARE(player.prefinishMark(), 1500ms);

    QVariant readBack;
    QVERIFY(QMetaObject::invokeMethod(object.get(), "readMilliseconds", Q_RETURN_ARG(QVariant, readBack)));
    QCOMPARE(readBack.toDouble(), 1500.0);
}

QTEST_GUILESS_MAIN(TestDragonDurationQml)
#include "test_dragonduration_qml.moc"
