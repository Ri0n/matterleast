#pragma once

#include <memory>
#include <vector>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantHash>

#include "plugins/api/CallsTypes.h"

class QAction;
class QWidget;
class QTemporaryDir;

namespace MatterLeast::PluginApi { class CallsProviderInterface; }

namespace Mattermost {

class Backend;
class AccountManager;
class PluginHost;

struct PluginInfo
{
    QString id;
    QString name;
    QString version;
    QString filePath;
    bool enabled = false;
};

struct IncomingCall {
    QString accountId;
    QString id;
    QString channelId;
    QString postId;
    QString threadId;
    QString ownerId;
};

class PluginManager final : public QObject
{
    Q_OBJECT
public:
    // Optional: tests may construct widgets without the application plugin service.
    static PluginManager* instance();
    static void restoreBackendConnections(Backend& backend);
    explicit PluginManager(AccountManager& accounts, QObject* parent = nullptr);
    ~PluginManager() override;
    // Called after application options/configuration are initialized.
    void initializePlugins();

    QList<QAction*> createConversationMenuActions(
        QObject* parent,
        const QString& conversationId,
        const QString& threadRootId);

    /** Attach enabled window-overlay extensions to the application window once. */
    void attachMainWindow(QWidget& window);

    QList<PluginInfo> pluginInfos() const;
    bool setPluginEnabled(const QString& pluginId, bool enabled);
    /** Descriptors include the persisted value; only enabled plugins expose settings. */
    QList<QVariantHash> pluginSettings(const QString& pluginId) const;
    bool setPluginSetting(const QString& pluginId,
                          const QString& key, const QVariant& value);
    QStringList loadedPluginIds() const;
    QStringList pluginSearchPaths() const;

    /** Calls capability is absent when no enabled Calls provider is loaded. */
    bool callsAvailable() const;
    MatterLeast::PluginApi::CallsSnapshot callsSnapshot() const;
    QString accountId(const Backend& backend) const;
    bool startCall(const QString& accountId, const QString& channelId);
    bool joinCall(const QString& accountId, const QString& channelId);
    bool leaveCall(const QString& accountId);
    bool setCallMicrophoneMuted(bool muted);
    QString incomingCallForPost(const QString& accountId,
                                const QString& postId) const;
    void answerIncomingCall(const QString& accountId,
                            const QString& callId, bool accept);

signals:
    void callsAvailabilityChanged(bool available);
    void callsSnapshotChanged();
    void incomingCallStarted(const IncomingCall& call);
    void incomingCallClosed(const QString& accountId, const QString& callId);

private:
    class ConversationHost;
    class CallsHost;
    struct IncomingInvitation {
        IncomingCall call;
        qint64 startAt = 0;
        bool pending = true;
        bool announced = false;
        bool checking = false;
    };

    static QString incomingKey(const QString& accountId, const QString& channelId);
    Backend* backendForAccount(const QString& accountId) const;
    void handleIncomingCallEvent(const QString& accountId,
                                 const QString& name, const QJsonObject& data,
                                 const QJsonObject& broadcast);
    void announceIncomingCall(const QString& accountId, const QString& channelId);
    void closeIncomingCall(const QString& accountId, const QString& channelId);
    void endIncomingCall(const QString& accountId, const QString& channelId);
    void clearIncomingCalls(const QString& accountId = {});
    void handleIncomingCallSnapshot();
    QHash<QString, IncomingInvitation> incomingCalls_;
    QString joiningIncomingCallId_;
    quint64 incomingCallsGeneration_ = 0; // Global plugin disable.
    QHash<QString, quint64> incomingCallsAccountGeneration_;

    void connectBackendSignals(Backend& backend);
    void attachWindowOverlay(PluginHost& host);
    void restorePluginSettings(const QString& pluginId, QObject& object);
    void loadStandardPlugins();
    bool loadPluginFile(const QString& filePath);
    void shutdown();
    MatterLeast::PluginApi::CallsProviderInterface* callsProvider() const;

    AccountManager& accounts_;
    QPointer<QWidget> _mainWindow;
    std::unique_ptr<ConversationHost> conversationHost_;
    std::unique_ptr<CallsHost> callsHost_;
    std::unique_ptr<QTemporaryDir> _verifiedPluginDirectory;
    std::vector<std::unique_ptr<PluginHost>> plugins_;
};

} // namespace Mattermost
