#include "Account.h"

#include "AccountManager.h"
#include "backend/Backend.h"

namespace Mattermost {

Account::Account(const QString& id, AccountManager* manager)
    : QObject(manager)
    , _accountId(id)
    , _manager(manager)
{
}

const WorkspaceLayout::Account* Account::record() const
{
    return _manager->layout().account(_accountId);
}

void Account::setState(AccountConnectionState state)
{
    if (_state == state)
        return;
    _state = state;
    emit stateChanged();
}

Backend* Account::createPendingBackend()
{
    if (_pending || _backend)
        return nullptr;
    _pending = new Backend(this);
    setState(AccountConnectionState::Connecting);
    return _pending.data();
}

void Account::cancelPending(AccountConnectionState nextState)
{
    QPointer<Backend> discarded = _pending;
    _pending.clear();
    if (discarded) {
        if (discarded->loggedIn())
            discarded->reset();
        else
            discarded->cancelPendingLogin();
        // The login callback may still be on the current stack.
        discarded->deleteLater();
    }
    setState(nextState);
}

bool Account::adoptBackend(Backend* backend)
{
    if (!backend || _backend || !backend->loggedIn())
        return false;
    if (_pending && _pending != backend)
        return false;
    if (_pending == backend)
        _pending.clear();
    backend->setParent(this);
    _backend = backend;
    setState(AccountConnectionState::Connected);
    return true;
}

void Account::releaseBackend()
{
    QPointer<Backend> released = _backend;
    _backend.clear();
    if (released)
        released->deleteLater();
}

} // namespace Mattermost
