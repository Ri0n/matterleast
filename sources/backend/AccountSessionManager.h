#pragma once

#include <memory>
#include <vector>

#include <QHash>
#include <QObject>
#include <QString>

namespace Mattermost {

class Backend;

// Owns the Backend lifetime of each login attempt and authenticated account.
// An authenticated account is identified by its normalized server base URL
// and server-assigned user ID, not by an access token or a mutable UI selection.
//
// The future account presentation owns its widgets separately: destroying a
// hidden workspace must never tear down another account's Backend.
class AccountSessionManager final : public QObject
{
    Q_OBJECT
public:
    explicit AccountSessionManager(QObject* parent = nullptr);
    ~AccountSessionManager() override;

    // Creates a non-authenticated Backend. Register it only after a successful
    // login, once its session origin and server user identity are known.
    Backend& createLoginBackend();
    bool registerAuthenticated(Backend& backend);
    bool unregisterAccount(Backend& backend);

    Backend* findAccount(const QString& serverBaseUrl,
                         const QString& serverUserId) const;
    Backend* activeBackend() const { return _activeBackend; }
    bool activate(Backend& backend);
    int registeredCount() const { return _registered.size(); }
    int backendCount() const { return static_cast<int>(_sessions.size()); }

    // Caller must release all UI referencing the Backend and log out (or
    // cancel its pending login) before removing it.
    bool removeBackend(Backend& backend);

signals:
    void accountRegistered(Backend* backend);
    void accountUnregistered(Backend* backend);
    void activeBackendChanged(Backend* backend);

private:
    struct Session;
    static QString identityKey(const QString& serverBaseUrl,
                               const QString& serverUserId);
    Session* sessionFor(const Backend* backend) const;
    void changeActiveBackend(Backend* backend);
    Backend* firstRegisteredBackend() const;

    std::vector<std::unique_ptr<Session>> _sessions;
    QHash<QString, Session*> _registered;
    Backend* _activeBackend = nullptr;
};

} // namespace Mattermost
