// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QtTest/QtTest>

#include <QFrame>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QPushButton>
#include <QToolButton>

#include "plugins/calls-gstreamer/CallsWindow.h"

using MatterLeast::PluginApi::CallsParticipant;
using MatterLeast::PluginApi::CallsSnapshot;
using MatterLeast::PluginApi::CallsState;

class CallsWindowTest final : public QObject
{
    Q_OBJECT
private slots:
    void groupTilesAndControls()
    {
        QList<bool> muteRequests;
        int leaves = 0;
        CallsWindow window(
            [&](bool muted) {
                muteRequests.append(muted);
                return true;
            },
            [&] { ++leaves; }, QStringLiteral("alice"));

        CallsSnapshot snapshot;
        snapshot.state = CallsState::Connecting;
        snapshot.channelId = QStringLiteral("channel-001");
        window.showForCall(snapshot, QStringLiteral("Engineering"));
        QCOMPARE(window.windowTitle(), QStringLiteral("Call — Engineering"));
        auto* end = window.findChild<QPushButton*>(QStringLiteral("callsEnd"));
        auto* mic = window.findChild<QToolButton*>(QStringLiteral("callsMicrophone"));
        QVERIFY(end);
        QVERIFY(mic);
        QVERIFY(!mic->icon().isNull());
        QVERIFY(mic->cursor().shape() == Qt::PointingHandCursor);
        QCOMPARE(end->text(), QStringLiteral("Cancel"));
        QVERIFY(!mic->isEnabled());

        snapshot.state = CallsState::Connected;
        snapshot.participants = {
            CallsParticipant {QStringLiteral("alice"), QStringLiteral("a"), false},
            CallsParticipant {QStringLiteral("bob"), QStringLiteral("b"), true}
        };
        window.updateSnapshot(snapshot);
        QVERIFY(mic->isEnabled());
        QVERIFY(mic->isChecked());
        QCOMPARE(end->text(), QStringLiteral("00:00:00"));
        auto tiles = window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile"));
        QCOMPARE(tiles.size(), 2);
        QFrame* aliceTile = nullptr;
        for (QFrame* tile : tiles) {
            if (tile->property("sessionId").toString() == QLatin1String("a")) {
                aliceTile = tile;
            }
        }
        QVERIFY(aliceTile);
        QVERIFY(aliceTile->property("localParticipant").toBool());
        QCOMPARE(aliceTile->findChild<QLabel*>(QStringLiteral("callsParticipantName"))->text(),
                 QStringLiteral("You"));

        // A late third participant adds one tile, without rebuilding existing
        // widgets or re-sending any microphone change.
        snapshot.participants.append(
            CallsParticipant {QStringLiteral("charlie"), QStringLiteral("c"), false});
        window.updateSnapshot(snapshot);
        QCOMPARE(window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).size(), 3);
        QVERIFY(window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).contains(aliceTile));
        QVERIFY(muteRequests.isEmpty());

        snapshot.participants.removeAt(1);
        window.updateSnapshot(snapshot);
        QCOMPARE(window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).size(), 2);
        QVERIFY(window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).contains(aliceTile));
        QVERIFY(muteRequests.isEmpty());

        QTest::mouseClick(mic, Qt::LeftButton);
        QCOMPARE(muteRequests, QList<bool> {true});
        snapshot.microphoneMuted = true;
        window.updateSnapshot(snapshot);
        QVERIFY(!mic->isChecked());
        QCOMPARE(muteRequests.size(), 1);

        window.close();
        QCOMPARE(leaves, 1);
    }

    void microphoneDeviceSelection()
    {
        QString chosen;
        int refreshes = 0;
        CallsWindow window([](bool) { return true; }, [] {});
        window.setMicrophoneDeviceHandlers(
            [&](const QString& id) { chosen = id; },
            [&] { ++refreshes; });
        CallsSnapshot state;
        state.state = CallsState::Connected;
        state.channelId = QStringLiteral("dm-1");
        window.showForCall(state, QStringLiteral("Direct"));
        auto* select = window.findChild<QToolButton*>(
            QStringLiteral("callsMicrophoneDevices"));
        QVERIFY(select);
        QVERIFY(select->isEnabled());
        QList<CallsMediaDevice> available {
            {QStringLiteral("microphone-one"), QStringLiteral("USB microphone"),
             CallsDeviceKind::Microphone},
            {QStringLiteral("microphone-two"), QStringLiteral("Laptop microphone"),
             CallsDeviceKind::Microphone}
        };
        window.setMicrophoneDevices(available, QStringLiteral("microphone-one"));
        QVERIFY(select->menu());
        emit select->menu()->aboutToShow();
        QCOMPARE(refreshes, 1);
        QCOMPARE(select->menu()->actions().size(), 4); // Default, separator, two devices
        QCOMPARE(select->menu()->actions().at(2)->text(), QStringLiteral("USB microphone"));
        QVERIFY(select->menu()->actions().at(2)->isChecked());
        select->menu()->actions().at(3)->trigger();
        QCOMPARE(chosen, QStringLiteral("microphone-two"));
        window.setMicrophoneDevices(available, chosen);
        emit select->menu()->aboutToShow();
        QVERIFY(select->menu()->actions().at(3)->isChecked());

        state.state = CallsState::Leaving;
        window.updateSnapshot(state);
        QVERIFY(!select->isEnabled());
    }

    void cameraControlsAndParticipantVideo()
    {
        QList<bool> cameraActions;
        QString selected;
        CallsWindow window([](bool) { return true; }, [] {});
        window.setCameraHandlers(
            [&](bool enabled) { cameraActions.append(enabled); return true; },
            [&](const QString& id) { selected = id; });
        window.setCameraDevices({
            {QStringLiteral("camera-one"), QStringLiteral("USB camera"),
             CallsDeviceKind::Camera}
        }, QStringLiteral("camera-one"));
        CallsSnapshot state;
        state.state = CallsState::Connected;
        state.channelId = QStringLiteral("dm-video");
        state.participants = {
            CallsParticipant{QStringLiteral("alice"), QStringLiteral("alice-session"), false},
            CallsParticipant{QStringLiteral("bob"), QStringLiteral("bob-session"), false}
        };
        window.showForCall(state, QStringLiteral("Video test"));
        auto* camera = window.findChild<QToolButton*>(QStringLiteral("callsCamera"));
        auto* selector = window.findChild<QToolButton*>(
            QStringLiteral("callsCameraDevices"));
        QVERIFY(camera);
        QVERIFY(selector);
        QVERIFY(!camera->isVisible());
        window.setCameraAvailable(true);
        QVERIFY(camera->isVisible());
        QVERIFY(camera->isEnabled());
        QVERIFY(!camera->icon().isNull());
        QTest::mouseClick(camera, Qt::LeftButton);
        QCOMPARE(cameraActions, QList<bool>{true});
        window.setCameraState(true);
        QVERIFY(camera->isChecked());
        QVERIFY(selector->menu());
        emit selector->menu()->aboutToShow();
        QCOMPARE(selector->menu()->actions().size(), 3);
        QVERIFY(selector->menu()->actions().at(2)->isChecked());
        selector->menu()->actions().first()->trigger();
        QCOMPARE(selected, QString());

        window.setParticipantSpeaking(QStringLiteral("bob-session"), true);
        window.setParticipantHandRaised(QStringLiteral("bob-session"), true);
        window.setParticipantScreenSharing(QStringLiteral("bob-session"), true);
        window.setParticipantCameraState(QStringLiteral("bob-session"), true);
        auto* tile = [&]() -> QFrame* {
            for (auto* candidate : window.findChildren<QFrame*>(
                     QStringLiteral("callsParticipantTile"))) {
                if (candidate->property("sessionId").toString()
                    == QLatin1String("bob-session")) return candidate;
            }
            return nullptr;
        }();
        QVERIFY(tile);
        auto* activity = tile->findChild<QLabel*>(
            QStringLiteral("callsParticipantActivity"));
        QVERIFY(activity);
        QVERIFY(activity->text().contains(QStringLiteral("Speaking")));
        QVERIFY(activity->text().contains(QStringLiteral("Hand raised")));
        QVERIFY(activity->text().contains(QStringLiteral("Sharing screen")));
        window.setParticipantSpeaking(QStringLiteral("bob-session"), false);
        window.setParticipantHandRaised(QStringLiteral("bob-session"), false);
        window.setParticipantScreenSharing(QStringLiteral("bob-session"), false);
        QVERIFY(!activity->isVisible());
        auto* video = tile->findChild<QLabel*>(
            QStringLiteral("callsParticipantVideo"));
        QVERIFY(video);
        QVERIFY(video->isVisible());
        QImage frame(320, 180, QImage::Format_RGB888);
        frame.fill(Qt::green);
        window.setParticipantVideo(QStringLiteral("bob-session"), frame);
        QVERIFY(!video->pixmap().isNull());
        window.setParticipantCameraState(QStringLiteral("bob-session"), false);
        QVERIFY(!video->isVisible());
        window.setCameraState(false);
        QVERIFY(!camera->isChecked());
    }

    void responsiveStageAndFailureClose()
    {
        int ends = 0;
        CallsWindow window([](bool) { return true; }, [&] { ++ends; });
        CallsSnapshot snapshot;
        snapshot.state = CallsState::Connecting;
        snapshot.channelId = QStringLiteral("channel-002");
        window.showForCall(snapshot, QStringLiteral("Design room"));
        auto* heading = window.findChild<QLabel*>(QStringLiteral("callsHeaderTitle"));
        auto* subtitle = window.findChild<QLabel*>(QStringLiteral("callsHeaderSubtitle"));
        auto* count = window.findChild<QLabel*>(QStringLiteral("callsParticipantCount"));
        auto* grid = window.findChild<QGridLayout*>(QStringLiteral("callsTileGrid"));
        auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("callsParticipantsScroll"));
        QVERIFY(heading);
        QVERIFY(subtitle);
        QVERIFY(count);
        QVERIFY(grid);
        QVERIFY(scroll);
        QCOMPARE(heading->text(), QStringLiteral("Design room"));
        QCOMPARE(subtitle->text(), QStringLiteral("Connecting…"));
        QVERIFY(!scroll->isVisible());

        snapshot.state = CallsState::Connected;
        snapshot.participants = {
            CallsParticipant {QStringLiteral("alice"), QStringLiteral("a"), false},
            CallsParticipant {QStringLiteral("bob"), QStringLiteral("b"), false},
            CallsParticipant {QStringLiteral("charlie"), QStringLiteral("c"), true}
        };
        window.updateSnapshot(snapshot);
        QCOMPARE(subtitle->text(), QStringLiteral("Audio call connected"));
        QCOMPARE(count->text(), QStringLiteral("3 participants"));
        QVERIFY(scroll->isVisible());
        QFrame* charlieTile = nullptr;
        for (auto* tile : window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile"))) {
            if (tile->property("sessionId").toString() == QLatin1String("c"))
                charlieTile = tile;
        }
        QVERIFY(charlieTile);
        auto charlieCell = [&]() {
            const int index = grid->indexOf(charlieTile);
            if (index < 0) return QPoint(-1, -1);
            int row, column, rowSpan, columnSpan;
            grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
            return QPoint(row, column);
        };
        // Resize must rearrange *existing* widgets instead of recreating them.
        window.resize(450, 410);
        QTRY_COMPARE(charlieCell(), QPoint(1, 0));
        window.resize(760, 410);
        QTRY_COMPARE(charlieCell(), QPoint(0, 2));
        QVERIFY(window.findChildren<QFrame*>(QStringLiteral("callsParticipantTile"))
                    .contains(charlieTile));

        QPalette colors = window.palette();
        colors.setColor(QPalette::Highlight, QColor(55, 110, 165));
        window.setPalette(colors);
        QCoreApplication::processEvents();
        QVERIFY(!window.findChild<QToolButton*>(QStringLiteral("callsMicrophone"))->icon().isNull());

        snapshot.state = CallsState::Failed;
        snapshot.errorMessage = QStringLiteral("Server rejected audio");
        window.updateSnapshot(snapshot);
        QVERIFY(!scroll->isVisible());
        auto* placeholder = window.findChild<QLabel*>(QStringLiteral("callsPlaceholder"));
        QVERIFY(placeholder);
        QCOMPARE(placeholder->text(), snapshot.errorMessage);
        auto* end = window.findChild<QPushButton*>(QStringLiteral("callsEnd"));
        QVERIFY(end);
        QCOMPARE(end->text(), QStringLiteral("Close"));
        QTest::mouseClick(end, Qt::LeftButton);
        QVERIFY(!window.isVisible());
        QCOMPARE(ends, 1); // A failed call still needs provider/media cleanup.
    }

    void errorAndIdle()
    {
        CallsWindow window([](bool) { return false; }, [] {});
        CallsSnapshot state;
        state.state = CallsState::Failed;
        state.channelId = QStringLiteral("broken");
        state.errorMessage = QStringLiteral("Server rejected call");
        window.showForCall(state, {});
        auto* end = window.findChild<QPushButton*>(QStringLiteral("callsEnd"));
        auto* hint = window.findChild<QLabel*>(QStringLiteral("callsPlaceholder"));
        QVERIFY(end);
        QVERIFY(hint);
        QCOMPARE(end->text(), QStringLiteral("Close"));
        QCOMPARE(hint->text(), QStringLiteral("Server rejected call"));
        window.updateSnapshot({});
        QVERIFY(!window.isVisible());
    }
};

QTEST_MAIN(CallsWindowTest)
#include "CallsWindowTest.moc"
