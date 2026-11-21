#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "workspaces/AccountConnectionState.h"
#include "workspaces/WorkspaceLayout.h"

namespace Mattermost {
class Backend;
class AccountManager;

// Durable account identity with an optional network session. Account remains
// alive when its Backend is absent, disconnected or being authenticated.
class Account final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(int connectionState READ connectionStateInt NOTIFY stateChanged)
public:
    Account(const QString& id, AccountManager* manager);

    QString id() const { return _id; }
    const WorkspaceLayout::Account* record() const;
    Backend* backend() const { return _backend.data(); }
    Backend* pendingBackend() const { return _pending.data(); }
    AccountConnectionState state() const { return _state; }
    int connectionStateInt() const { return static_cast<int>(_state); }

signals:
    void stateChanged();

private:
    friend class AccountManager;
    void setState(AccountConnectionState state);
    Backend* createPendingBackend();
    void cancelPending(AccountConnectionState nextState);
    bool adoptBackend(Backend* backend);
    void releaseBackend();

    const QString _id;
    AccountManager* const _manager;
    AccountConnectionState _state = AccountConnectionState::Disconnected;
    QPointer<Backend> _pending;
    QPointer<Backend> _backend;
};

} // namespace Mattermost
