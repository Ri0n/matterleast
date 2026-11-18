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
    bool authenticated() const override { return true; }
    QString localUserId() const override { return QStringLiteral("test-local-user"); }

    void request(const QString&, const QString&, const QJsonObject&,
                 QObject*, Reply callback) override
    {
        ++requests;
        callback(200, QJsonObject{{QStringLiteral("ok"), true}});
    }

    void providerChanged() override { ++changes; }

    int changes = 0;
    int requests = 0;
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
        QVERIFY(provider->joinCall(QStringLiteral("channel-123")));
        QVERIFY(!provider->startCall(QStringLiteral("other-channel")));
        QCOMPARE(provider->snapshot().state,
                 MatterLeast::PluginApi::CallsState::Connected);
        QCOMPARE(provider->snapshot().channelId, QStringLiteral("channel-123"));
        QVERIFY(provider->setMicrophoneMuted(true));
        QVERIFY(provider->snapshot().microphoneMuted);
        QVERIFY(calls.changes >= 2);

        provider->onServerEvent(
            QStringLiteral("custom_calls_test"),
            QJsonObject{{QStringLiteral("user_id"), QStringLiteral("remote-user")}},
            QJsonObject());
        QCOMPARE(provider->snapshot().participants.size(), 1);
        QCOMPARE(provider->snapshot().participants.first().userId,
                 QStringLiteral("remote-user"));

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
        QVERIFY(provider->startCall(QStringLiteral("another-channel")));
        provider->onConnectionChanged(false);
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
