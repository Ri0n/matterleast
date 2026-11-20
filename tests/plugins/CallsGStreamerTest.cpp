#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QPushButton>

#include "plugins/PluginHost.h"
#include "plugins/api/CallsHostInterface.h"
#include "plugins/api/CallsProviderInterface.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"

namespace {

class EmptyConversationHost final
    : public MatterLeast::PluginApi::ConversationAccessingHost
{
public:
    QString displayName(const QString&) const override { return {}; }
    QString reference(const QString&) const override { return {}; }
    void open(const QString&, const QString&) override {}
    void openInNewTab(const QString&, const QString&) override {}
};

class FakeCallsHost final : public MatterLeast::PluginApi::CallsHostInterface
{
public:
    bool authenticated() const override { return signedIn; }
    QString localUserId() const override { return QStringLiteral("user-A"); }
    QString connectionId() const override { return QStringLiteral("conn-A"); }

    void request(const QString& method, const QString& path,
                 const QJsonObject&, QObject* context, Reply callback) override
    {
        lastRequestMethod = method;
        lastRequestPath = path;
        ++configRequests;
        const bool valid = method == QLatin1String("GET")
            && path == QLatin1String("plugins/com.mattermost.calls/config");
        QTimer::singleShot(0, context, [this, callback, valid] {
            ++configReplies;
            callback(valid ? configStatus : 404,
                     {{QStringLiteral("ICEServersConfigs"), QJsonArray()},
                      {QStringLiteral("EnableVideo"), true}});
        });
    }

    bool sendAction(const QString& action, const QJsonObject& body) override
    {
        actions.append(action);
        lastActionData = body;
        return true;
    }

    bool sendSdp(const QJsonObject& description) override
    {
        if (description.value(QStringLiteral("type")) == QLatin1String("offer")
            && !description.value(QStringLiteral("sdp")).toString().isEmpty()) {
            offers.append(description);
        }
        return true;
    }

    void providerChanged() override { ++changed; }

    bool signedIn = true;
    int configStatus = 200;
    int configRequests = 0;
    int configReplies = 0;
    int changed = 0;
    QString lastRequestMethod;
    QString lastRequestPath;
    QStringList actions;
    QJsonObject lastActionData;
    QList<QJsonObject> offers;
};

} // namespace

class CallsGStreamerTest final : public QObject
{
    Q_OBJECT
private slots:
    void loadsAndCreatesRealLocalOpusOffer()
    {
        const QString path = qEnvironmentVariable("MATTERLEAST_CALLS_PLUGIN_PATH");
        QVERIFY2(!path.isEmpty(), "Missing Calls GStreamer test plugin");
        EmptyConversationHost conversations;
        FakeCallsHost calls;
        Mattermost::PluginHost loader(path, conversations, &calls);
        QVERIFY(loader.hasValidMetadata());
        QVERIFY(loader.loadAndEnable());

        auto* provider = qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(
            loader.instance());
        QVERIFY(provider);
        struct ScopedPluginCleanup {
            MatterLeast::PluginApi::CallsProviderInterface* provider;
            Mattermost::PluginHost* loader;
            ~ScopedPluginCleanup()
            {
                if (!loader->instance()) {
                    return;
                }
                if (provider->snapshot().state
                    != MatterLeast::PluginApi::CallsState::Idle) {
                    provider->leaveCall();
                }
                loader->disableAndUnload();
            }
        } cleanup{provider, &loader};
        auto* menu = qobject_cast<MatterLeast::PluginApi::ConversationMenuInterface*>(
            loader.instance());
        QVERIFY(menu);
        QCOMPARE(menu->conversationMenuActions(QStringLiteral("channel-A"), {}).size(), 1);

        QVERIFY(provider->joinCall(QStringLiteral("channel-A")));
        QWidget* window = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QLatin1String("callsWindow")) {
                window = widget;
                break;
            }
        }
        QVERIFY(window);
        QCOMPARE(window->windowTitle(), QStringLiteral("Call — channel-A"));
        auto* end = window->findChild<QPushButton*>(QStringLiteral("callsEnd"));
        QVERIFY(end);
        QCOMPARE(end->text(), QStringLiteral("Cancel"));
        QCOMPARE(calls.configRequests, 1);
        QCOMPARE(calls.lastRequestMethod, QStringLiteral("GET"));
        QCOMPARE(calls.lastRequestPath,
                 QStringLiteral("plugins/com.mattermost.calls/config"));
        QTRY_COMPARE_WITH_TIMEOUT(calls.configReplies, 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(calls.actions.contains(
            QStringLiteral("custom_com.mattermost.calls_join")), 3000);
        QCOMPARE(calls.lastActionData.value(QStringLiteral("channelID")).toString(),
                 QStringLiteral("channel-A"));

        // No RTC session may be constructed before server join acknowledgement.
        QCOMPARE(provider->snapshot().state,
                 MatterLeast::PluginApi::CallsState::Connecting);
        QVERIFY(calls.offers.isEmpty());
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_join"),
            {{QStringLiteral("connID"), QStringLiteral("conn-A")}}, {});
        QTRY_VERIFY_WITH_TIMEOUT(!calls.offers.isEmpty(), 18000);

        const QString sdp = calls.offers.first()
                            .value(QStringLiteral("sdp")).toString();
        QVERIFY(sdp.contains(QStringLiteral("m=audio")));
        QVERIFY(sdp.contains(QStringLiteral("m=video")));
        QVERIFY(sdp.contains(QStringLiteral("m=application")));
        const QString videoSection = sdp.section(
            QStringLiteral("m=video"), 1).section(QStringLiteral("m=application"), 0, 0);
        QVERIFY2(videoSection.contains(QStringLiteral(" msid:"))
                     || videoSection.contains(QStringLiteral("a=msid:")),
                 qPrintable(videoSection));
        QVERIFY(sdp.contains(QStringLiteral("opus/48000/2")));

        // Official Calls sends a full state snapshot after join. A late
        // participant did not receive the existing peers' earlier joins.
        const QJsonObject callState {
            {QStringLiteral("id"), QStringLiteral("call-abc")},
            {QStringLiteral("sessions"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("session_id"), QStringLiteral("session-alice")},
                    {QStringLiteral("user_id"), QStringLiteral("alice")},
                    {QStringLiteral("unmuted"), true}
                },
                QJsonObject {
                    {QStringLiteral("session_id"), QStringLiteral("session-bob")},
                    {QStringLiteral("user_id"), QStringLiteral("bob")},
                    {QStringLiteral("unmuted"), false}
                }
            }}
        };
        const QJsonObject stateEvent {
            {QStringLiteral("channel_id"), QStringLiteral("channel-A")},
            {QStringLiteral("call"),
             QString::fromUtf8(QJsonDocument(callState).toJson(QJsonDocument::Compact))}
        };
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_call_state"),
            stateEvent, {});
        QCOMPARE(provider->snapshot().callId, QStringLiteral("call-abc"));
        QCOMPARE(provider->snapshot().participants.size(), 2);
        QCOMPARE(window->findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).size(), 2);
        QCOMPARE(provider->snapshot().participants.at(0).userId, QStringLiteral("alice"));
        QCOMPARE(provider->snapshot().participants.at(1).userId, QStringLiteral("bob"));
        QVERIFY(provider->snapshot().participants.at(1).microphoneMuted);
        auto* bobTile = [&]() -> QFrame* {
            for (QFrame* tile : window->findChildren<QFrame*>(
                     QStringLiteral("callsParticipantTile"))) {
                if (tile->property("sessionId").toString()
                    == QStringLiteral("session-bob")) return tile;
            }
            return nullptr;
        }();
        QVERIFY(bobTile);
        auto* bobMuteLabel = bobTile->findChild<QLabel*>(
            QStringLiteral("callsParticipantMute"));
        QVERIFY(bobMuteLabel);
        QCOMPARE(bobMuteLabel->text(), QStringLiteral("Microphone off"));

        // The official server emits userID (camelCase) for mute changes,
        // while join events and call_state use user_id.
        const QJsonObject bobIdentity {
            {QStringLiteral("userID"), QStringLiteral("bob")},
            {QStringLiteral("session_id"), QStringLiteral("session-bob")}
        };
        const QJsonObject inChannel {
            {QStringLiteral("channel_id"), QStringLiteral("channel-A")}
        };
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_unmuted"),
            bobIdentity, inChannel);
        QVERIFY(!provider->snapshot().participants.at(1).microphoneMuted);
        QCOMPARE(bobMuteLabel->text(), QStringLiteral("Microphone on"));

        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_muted"),
            bobIdentity, {{QStringLiteral("channel_id"),
                          QStringLiteral("unrelated-channel")}});
        QVERIFY(!provider->snapshot().participants.at(1).microphoneMuted);
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_muted"),
            {{QStringLiteral("userID"), QStringLiteral("someone-else")},
             {QStringLiteral("session_id"), QStringLiteral("session-bob")}},
            inChannel);
        QVERIFY(!provider->snapshot().participants.at(1).microphoneMuted);
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_muted"),
            bobIdentity, inChannel);
        QVERIFY(provider->snapshot().participants.at(1).microphoneMuted);
        QCOMPARE(bobMuteLabel->text(), QStringLiteral("Microphone off"));

        // A call_state from another channel must not overwrite our state.
        QJsonObject foreignState = stateEvent;
        foreignState.insert(QStringLiteral("channel_id"),
                            QStringLiteral("unrelated-channel"));
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_call_state"),
            foreignState, {});
        QCOMPARE(provider->snapshot().participants.size(), 2);

        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_joined"),
            {{QStringLiteral("user_id"), QStringLiteral("charlie")},
             {QStringLiteral("session_id"), QStringLiteral("session-charlie")}},
            {{QStringLiteral("channel_id"), QStringLiteral("channel-A")}});
        QCOMPARE(provider->snapshot().participants.size(), 3);
        QCOMPARE(window->findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).size(), 3);
        QVERIFY(provider->snapshot().participants.at(2).microphoneMuted);
        provider->onServerEvent(
            QStringLiteral("custom_com.mattermost.calls_user_left"),
            {{QStringLiteral("user_id"), QStringLiteral("charlie")},
             {QStringLiteral("session_id"), QStringLiteral("session-charlie")}},
            {{QStringLiteral("channel_id"), QStringLiteral("channel-A")}});
        QCOMPARE(provider->snapshot().participants.size(), 2);
        QCOMPARE(window->findChildren<QFrame*>(QStringLiteral("callsParticipantTile")).size(), 2);

        // Native plugin must refuse to unload with a live media pipeline.
        QVERIFY(!loader.disableAndUnload());
        QVERIFY(provider->leaveCall());
        QCOMPARE(provider->snapshot().state,
                 MatterLeast::PluginApi::CallsState::Idle);
        QVERIFY(loader.disableAndUnload());
        QVERIFY(!loader.isEnabled());
    }

    void missingCallsServerEndpoint()
    {
        EmptyConversationHost conversations;
        FakeCallsHost calls;
        calls.configStatus = 404;
        Mattermost::PluginHost loader(
            qEnvironmentVariable("MATTERLEAST_CALLS_PLUGIN_PATH"),
            conversations, &calls);
        QVERIFY(loader.loadAndEnable());
        auto* provider = qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(
            loader.instance());
        QVERIFY(provider);
        QVERIFY(provider->joinCall(QStringLiteral("channel-A")));
        QTRY_COMPARE_WITH_TIMEOUT(calls.configReplies, 1, 3000);
        QCOMPARE(provider->snapshot().state,
                 MatterLeast::PluginApi::CallsState::Failed);
        QVERIFY(provider->snapshot().errorMessage.contains(QStringLiteral("404")));
        QVERIFY(provider->snapshot().errorMessage.contains(QStringLiteral("/plugins/")));
        QVERIFY(!calls.actions.contains(QStringLiteral("custom_com.mattermost.calls_join")));
        QVERIFY(calls.offers.isEmpty());
        QVERIFY(provider->leaveCall());
        QVERIFY(loader.disableAndUnload());
    }

    void missingSessionIsNotAConnection()
    {
        EmptyConversationHost conversations;
        FakeCallsHost calls;
        calls.signedIn = false;
        Mattermost::PluginHost loader(
            qEnvironmentVariable("MATTERLEAST_CALLS_PLUGIN_PATH"),
            conversations, &calls);
        QVERIFY(loader.loadAndEnable());
        auto* provider = qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(
            loader.instance());
        QVERIFY(provider);
        QVERIFY(!provider->joinCall(QStringLiteral("channel-A")));
        QCOMPARE(calls.configRequests, 0);
        QVERIFY(loader.disableAndUnload());
    }
};

// Async Calls HTTP callbacks, websocket events and GStreamer worker notifications
// require QApplication and its event dispatcher, not APPLESS_MAIN.
QTEST_MAIN(CallsGStreamerTest)
#include "CallsGStreamerTest.moc"
