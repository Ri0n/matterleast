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
    static PluginManager& instance(Backend& backend);
    static void restoreBackendConnections(Backend& backend);
    ~PluginManager() override;

    QList<QAction*> createConversationMenuActions(
        QObject* parent,
        const QString& conversationId,
        const QString& threadRootId);

    /** Attach enabled window-overlay extensions to this main window. */
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
    bool startCall(const QString& channelId);
    bool joinCall(const QString& channelId);
    bool leaveCall();
    bool setCallMicrophoneMuted(bool muted);
    QString incomingCallForPost(const QString& postId) const;
    void answerIncomingCall(const QString& callId, bool accept);

signals:
    void callsAvailabilityChanged(bool available);
    void callsSnapshotChanged();
    void incomingCallStarted(const IncomingCall& call);
    void incomingCallClosed(const QString& callId);

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

    void handleIncomingCallEvent(const QString& name, const QJsonObject& data,
                                 const QJsonObject& broadcast);
    void announceIncomingCall(const QString& channelId);
    void closeIncomingCall(const QString& channelId);
    void endIncomingCall(const QString& channelId);
    void clearIncomingCalls();
    void handleIncomingCallSnapshot();
    QHash<QString, IncomingInvitation> incomingCalls_;
    QString joiningIncomingCallId_;
    quint64 incomingCallsGeneration_ = 0;

    explicit PluginManager(Backend& backend);
    void connectBackendSignals();
    void attachWindowOverlay(PluginHost& host);
    void restorePluginSettings(const QString& pluginId, QObject& object);
    void loadStandardPlugins();
    bool loadPluginFile(const QString& filePath);
    void shutdown();
    MatterLeast::PluginApi::CallsProviderInterface* callsProvider() const;

    Backend& backend_;
    QPointer<QWidget> _mainWindow;
    std::unique_ptr<ConversationHost> conversationHost_;
    std::unique_ptr<CallsHost> callsHost_;
    std::unique_ptr<QTemporaryDir> _verifiedPluginDirectory;
    std::vector<std::unique_ptr<PluginHost>> plugins_;
};

} // namespace Mattermost
