#include <QtTest/QtTest>

#include "plugins/PluginHost.h"
#include "plugins/api/CallsHostInterface.h"
#include "plugins/api/CallsProviderInterface.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/PluginSettingsInterface.h"

namespace {

class FakeConversationHost final
    : public MatterLeast::PluginApi::ConversationAccessingHost
{
public:
    QString displayName(const QString& conversationId) const override
    {
        return QStringLiteral("display:") + conversationId;
    }

    QString reference(const QString& conversationId) const override
    {
        return QStringLiteral("ref:") + conversationId;
    }

    void open(const QString& conversationId,
              const QString& threadRootId) override
    {
        openedConversation = conversationId;
        openedThread = threadRootId;
    }

    void openInNewTab(const QString& conversationId,
                      const QString& threadRootId) override
    {
        tabConversation = conversationId;
        tabThread = threadRootId;
    }

    QString openedConversation;
    QString openedThread;
    QString tabConversation;
    QString tabThread;
};

class FakeCallsHost final : public MatterLeast::PluginApi::CallsHostInterface
{
public:
    bool authenticated(const QString& accountId) const override
    { return signedIn && accountId == QLatin1String("account-1"); }
    QString localUserId(const QString&) const override
    { return QStringLiteral("test-local-user"); }
    QString selectedAccountId() const override
    { return QStringLiteral("account-1"); }
    QString conversationDisplayName(const QString&, const QString& channelId) const override
    { return channelId; }
    QString userDisplayName(const QString&, const QString& userId) const override
    { return QStringLiteral("name:") + userId; }
    QString connectionId(const QString&) const override
    { return QStringLiteral("test-connection"); }

    bool sendSdp(const QString& accountId, const QJsonObject& description) override
    {
        lastAccountId = accountId;
        lastSdp = description;
        return true;
    }

    bool sendAction(const QString& accountId, const QString& action,
                    const QJsonObject& data) override
    {
        if (!action.startsWith(QStringLiteral("custom_com.mattermost.calls_"))) {
            return false;
        }
        lastAccountId = accountId;
        lastAction = action;
        lastActionChannelId = data.value(QStringLiteral("channelID")).toString();
        ++actions;
        return true;
    }

    void request(const QString&, const QString&, const QString&, const QJsonObject&,
                 QObject*, Reply callback) override
    {
        ++requests;
        callback(200, QJsonObject{{QStringLiteral("ok"), true}});
    }

    void providerChanged() override { ++changes; }

    bool signedIn = true;
    QJsonObject lastSdp;
    int changes = 0;
    int requests = 0;
    int actions = 0;
    QString lastAction;
    QString lastAccountId;
    QString lastActionChannelId;
};

} // namespace

class PluginHostTest final : public QObject
{
    Q_OBJECT

private slots:
    void supportsCallsProviderLifecycleAndMediaIndependentAbi()
    {
        const QString path = qEnvironmentVariable("MATTERLEAST_TEST_CALLS_PLUGIN_PATH");
        QVERIFY2(!path.isEmpty(), "MATTERLEAST_TEST_CALLS_PLUGIN_PATH is not set");

        FakeConversationHost conversation;
        FakeCallsHost calls;

        // An optional capability must not load without its required host.
        {
            Mattermost::PluginHost missingHost(path, conversation);
            QVERIFY(missingHost.hasValidMetadata());
            QVERIFY(!missingHost.loadAndEnable());
            QVERIFY(!missingHost.isEnabled());
        }

        Mattermost::PluginHost plugin(path, conversation, &calls);
        QVERIFY(plugin.hasValidMetadata());
        QVERIFY(plugin.loadAndEnable());
        QVERIFY(plugin.isEnabled());

        auto* provider = qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(
            plugin.instance());
        QVERIFY(provider);
        QCOMPARE(provider->snapshot().state, MatterLeast::PluginApi::CallsState::Idle);
        calls.signedIn = false;
        QVERIFY(!provider->joinCall(QStringLiteral("account-1"), QStringLiteral("channel-123")));
        QVERIFY(plugin.isEnabled()); // A missing login must not hide the plugin.
        calls.signedIn = true;
        QVERIFY(provider->joinCall(QStringLiteral("account-1"), QStringLiteral("channel-123")));
        QCOMPARE(calls.actions, 1);
        QCOMPARE(calls.lastAccountId, QStringLiteral("account-1"));
        QCOMPARE(provider->snapshot().accountId, QStringLiteral("account-1"));
        QCOMPARE(calls.lastAction, QStringLiteral("custom_com.mattermost.calls_join"));
        QCOMPARE(calls.lastActionChannelId, QStringLiteral("channel-123"));
        QVERIFY(!provider->startCall(QStringLiteral("account-2"), QStringLiteral("other-channel")));
        QCOMPARE(provider->snapshot().state,
                 MatterLeast::PluginApi::CallsState::Connected);
        QCOMPARE(provider->snapshot().channelId, QStringLiteral("channel-123"));
        QVERIFY(provider->setMicrophoneMuted(true));
        QVERIFY(provider->snapshot().microphoneMuted);
        QVERIFY(calls.changes >= 2);

        provider->onServerEvent(
            QStringLiteral("account-1"),
            QStringLiteral("custom_com.mattermost.calls_test"),
            QJsonObject{{QStringLiteral("user_id"), QStringLiteral("remote-user")}},
            QJsonObject());
        QCOMPARE(provider->snapshot().participants.size(), 1);
        QCOMPARE(provider->snapshot().participants.first().userId,
                 QStringLiteral("remote-user"));
        QCOMPARE(provider->snapshot().participants.first().displayName,
                 QStringLiteral("name:remote-user"));
        provider->onServerEvent(QStringLiteral("account-2"),
            QStringLiteral("custom_com.mattermost.calls_test"),
            QJsonObject{{QStringLiteral("user_id"), QStringLiteral("wrong-server")}}, {});
        QCOMPARE(provider->snapshot().participants.size(), 1);
        provider->onConnectionChanged(QStringLiteral("account-2"), false);
        QCOMPARE(provider->snapshot().state, MatterLeast::PluginApi::CallsState::Connected);

        // Critical: QPluginLoader may not unload code while a call is active.
        QVERIFY(!plugin.disableAndUnload());
        QVERIFY(plugin.isEnabled());
        QVERIFY(plugin.instance() != nullptr);

        QVERIFY(provider->leaveCall());
        QCOMPARE(provider->snapshot().state, MatterLeast::PluginApi::CallsState::Idle);
        QVERIFY(plugin.disableAndUnload());
        QVERIFY(!plugin.isEnabled());
        QVERIFY(plugin.instance() == nullptr);
        QVERIFY(plugin.loadAndEnable());
        provider = qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(
            plugin.instance());
        QVERIFY(provider);
        QCOMPARE(provider->snapshot().state, MatterLeast::PluginApi::CallsState::Idle);
        QVERIFY(provider->startCall(QStringLiteral("account-1"), QStringLiteral("another-channel")));
        provider->onConnectionChanged(QStringLiteral("account-1"), false);
        QCOMPARE(provider->snapshot().state, MatterLeast::PluginApi::CallsState::Failed);
        QVERIFY(!provider->snapshot().errorMessage.isEmpty());
        QVERIFY(provider->leaveCall());
        QVERIFY(plugin.disableAndUnload());
        QVERIFY(!plugin.isEnabled());
    }

    void supportsEnableDisableAndReenableLifecycle()
    {
        const QString pluginPath = qEnvironmentVariable("MATTERLEAST_TEST_PLUGIN_PATH");
        QVERIFY2(!pluginPath.isEmpty(), "MATTERLEAST_TEST_PLUGIN_PATH is not set");

        FakeConversationHost conversationHost;
        Mattermost::PluginHost host(pluginPath, conversationHost);

        QVERIFY(host.hasValidMetadata());
        QCOMPARE(host.id(),
                 QStringLiteral("io.github.Ri0n.MatterLeast.tests.conversation"));
        QCOMPARE(host.name(), QStringLiteral("Conversation API test plugin"));
        QCOMPARE(host.version(), QStringLiteral("1.0.0"));
        QVERIFY(host.loadAndEnable([](QObject& object) {
            auto* settings = qobject_cast<
                MatterLeast::PluginApi::PluginSettingsInterface*>(&object);
            QVERIFY(settings);
            QVERIFY(settings->setSettingValue(QStringLiteral("probe"), 42));
        }));
        QVERIFY(host.isEnabled());
        QCOMPARE(host.instance()->property("probeAtEnable").toInt(), 42);

        auto* extension = qobject_cast<
            MatterLeast::PluginApi::ConversationMenuInterface*>(host.instance());
        QVERIFY(extension);

        const auto actions = extension->conversationMenuActions(
            QStringLiteral("channel-1"), QStringLiteral("root-1"));
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.front().value(QStringLiteral("id")).toString(),
                 QStringLiteral("probe.open"));
        QVERIFY(actions.front().value(QStringLiteral("enabled")).toBool());

        extension->conversationMenuActionTriggered(
            QStringLiteral("probe.open"),
            QStringLiteral("channel-1"),
            QStringLiteral("root-1"),
            false);
        QCOMPARE(conversationHost.openedConversation,
                 QStringLiteral("channel-1"));
        QCOMPARE(conversationHost.openedThread, QStringLiteral("root-1"));

        QVERIFY(host.disableAndUnload());
        QVERIFY(!host.isEnabled());
        QVERIFY(host.instance() == nullptr);

        QVERIFY(host.loadAndEnable());
        QVERIFY(host.isEnabled());
        QVERIFY(host.instance() != nullptr);
        QCOMPARE(host.instance()->property("probeAtEnable").toInt(), 10);
        QVERIFY(host.disableAndUnload());
        QVERIFY(!host.isEnabled());
    }
};

QTEST_APPLESS_MAIN(PluginHostTest)
#include "PluginHostTest.moc"
