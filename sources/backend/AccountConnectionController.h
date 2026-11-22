#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace Mattermost {

// One account-scoped policy owner for initial session restoration, WebSocket
// recovery and the decision whether a resumed event stream is trustworthy.
// Backend and WebSocketConnector remain transport/protocol implementations.
// A single timer serves all accounts and both phases; no waiting Backend is
// retained when saved-session verification fails.
class AccountConnectionController final : public QObject
{
    Q_OBJECT
public:
    explicit AccountConnectionController(QObject* parent = nullptr);
    ~AccountConnectionController() override;

    void startSavedSession(const QString& accountId);
    void savedSessionFailed(const QString& accountId);
    void websocketUnavailable(const QString& accountId);
    void attemptStarted(const QString& accountId);
    void streamVerified(const QString& accountId, bool isReconnect,
                        bool continuityVerified);
    void cancel(const QString& accountId);
    void resumeOnNetworkChange();
    bool isScheduled(const QString& accountId) const;
    int scheduledCount() const { return _pending.size(); }

signals:
    void savedSessionDue(const QStringList& accountIds);
    void webSocketDue(const QStringList& accountIds);
    // Emitted only after both Mattermost hello and authentication succeed.
    // A new server stream cannot be treated as a lossless resume.
    void replayResolved(const QString& accountId, bool continuityVerified);
    void networkChanged();

private:
    enum class Kind { SavedSession, WebSocket };
    struct Pending {
        Kind kind;
        qint64 deadline;
    };

    void schedule(const QString& accountId, Kind kind);
    void arm();
    void deliverDue();

    QElapsedTimer _clock;
    QTimer _timer;
    QHash<QString, Pending> _pending;
    QHash<QString, int> _failedCycles;
#ifdef Q_OS_LINUX
    int _routeChangeSocket = -1;
#endif
};

} // namespace Mattermost
