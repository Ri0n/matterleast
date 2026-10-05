#include <QtTest/QtTest>

#include "plugins/PluginHost.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"

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

} // namespace

class PluginHostTest final : public QObject
{
    Q_OBJECT

private slots:
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
        QVERIFY(host.loadAndEnable());
        QVERIFY(host.isEnabled());

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
        QVERIFY(host.disableAndUnload());
        QVERIFY(!host.isEnabled());
    }
};

QTEST_APPLESS_MAIN(PluginHostTest)
#include "PluginHostTest.moc"
