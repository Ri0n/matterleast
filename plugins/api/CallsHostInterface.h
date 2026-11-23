#pragma once

#include <functional>

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

class QObject;

namespace MatterLeast::PluginApi {

// The host owns credentials and the Mattermost connection. The provider
// receives neither a session token nor a pointer to MatterLeast's Backend.
class CallsHostInterface
{
public:
    using Reply = std::function<void(int httpStatus, const QJsonObject& body)>;

    virtual ~CallsHostInterface() = default;

    virtual bool authenticated(const QString& accountId) const = 0;
    virtual QString localUserId(const QString& accountId) const = 0;
    virtual QString selectedAccountId() const = 0;
    // Resolve names from the account's existing cached conversation/user data.
    virtual QString conversationDisplayName(const QString& accountId,
                                            const QString& channelId) const = 0;
    virtual QString userDisplayName(const QString& accountId,
                                    const QString& userId) const = 0;

    // Send a Mattermost Calls WebSocket action through the host's existing
    // authenticated and sequenced connection. Only the official Calls
    // custom_com.mattermost.calls_* action namespace is accepted.
    virtual bool sendAction(const QString& accountId, const QString& action,
                            const QJsonObject& data) = 0;

    // Calls API path relative to server root, e.g.
    // plugins/com.mattermost.calls/config. It is NOT under /api/v4/.
    // Only GET/POST/PUT are
    // supported. A request is discarded after 'context' is destroyed; pass
    // the provider's QObject as context to avoid callbacks after unloading.
    virtual void request(const QString& accountId,
                         const QString& method,
                         const QString& relativePath,
                         const QJsonObject& body,
                         QObject* context, Reply completion) = 0;

    // Notify the main application that snapshot() has changed.
    virtual void providerChanged() = 0;

    // Version 2 uses account-qualified operations. The original 1.0 ABI must
    // not be loaded against these changed virtual-method signatures.
    virtual QString connectionId(const QString& accountId) const = 0;

    // The server accepts SDP only as binary MessagePack with a compressed
    // zlib SDP payload. The host owns framing and WebSocket sequencing.
    virtual bool sendSdp(const QString& accountId,
                         const QJsonObject& description) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CALLS_HOST_IID "io.github.Ri0n.MatterLeast.CallsHost/2.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::CallsHostInterface,
                    MATTERLEAST_CALLS_HOST_IID)
