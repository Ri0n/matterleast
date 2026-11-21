#include "AccountManager.h"

#include <QTimer>
#include <QUrl>
#include <utility>

#include "Account.h"
#include "backend/AccountIdentity.h"
#include "backend/Backend.h"
#include "backend/SessionTokenStore.h"
#include "backend/types/BackendLoginData.h"
#include "backend/types/BackendUser.h"
#include "options/MLOptions.h"

namespace Mattermost {

AccountManager::AccountManager(QObject* parent)
    : QObject(parent)
{
}

bool AccountManager::load(const QByteArray& json, QString* error)
{
    // Do not overwrite live account/session objects with a new layout.
    if (!_byId.isEmpty())
        return false;

    WorkspaceLayout parsed;
    if (!json.isEmpty()) {
        const auto restored = WorkspaceLayout::fromJson(json, error);
        if (!restored)
            return false;
        parsed = *restored;
    }
    _layout = std::move(parsed);
    for (const auto& record : _layout.accounts)
        ensureAccount(record.id);

    for (const auto& view : _layout.views) {
        if (view.id == _layout.activeViewId && !view.accountIds.isEmpty()) {
            _selectedId = view.accountIds.first();
            break;
        }
    }
    rebuildModel();
    if (error)
        error->clear();
    return true;
}

bool AccountManager::loadFromOptions(QString* error)
{
    const QByteArray json = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("workspaces/layout"));
    if (!load(json, error)) {
        _invalidCatalog = true;
        return false;
    }
    return true;
}

void AccountManager::persist() const
{
    if (_invalidCatalog || _layout.accounts.isEmpty())
        return;
    const QByteArray json = _layout.toJson();
    if (!json.isEmpty())
        MLOptions::instance()->setValue<QByteArray>(
            QStringLiteral("workspaces/layout"), json);
}

Account* AccountManager::ensureAccount(const QString& id)
{
    if (Account* existing = account(id))
        return existing;
    if (!_layout.account(id))
        return nullptr;
    auto* added = new Account(id, this);
    _byId.insert(id, added);
    connect(added, &Account::stateChanged, this,
            [this] { rebuildModel(); });
    return added;
}

QVector<Account*> AccountManager::allAccounts() const
{
    QVector<Account*> result;
    result.reserve(_layout.accounts.size());
    for (const auto& record : _layout.accounts)
        if (Account* ptr = account(record.id))
            result.append(ptr);
    return result;
}

QVector<Backend*> AccountManager::registeredBackends() const
{
    QVector<Backend*> result;
    for (Account* item : allAccounts())
        if (item->backend() && item->backend()->loggedIn()
            && item->state() == AccountConnectionState::Connected)
            result.append(item->backend());
    return result;
}

Backend* AccountManager::activeBackend() const
{
    Account* item = account(_selectedId);
    return item && item->state() == AccountConnectionState::Connected
        && item->backend() && item->backend()->loggedIn()
        ? item->backend() : nullptr;
}

bool AccountManager::selectAccount(const QString& id)
{
    if (!account(id) || !_layout.selectAccount(id))
        return false;
    if (_selectedId == id)
        return true;
    _selectedId = id;
    rebuildModel();
    persist();
    emit activeBackendChanged(activeBackend());
    return true;
}

Account* AccountManager::accountForBackend(const Backend* backend) const
{
    if (!backend)
        return nullptr;
    for (Account* item : allAccounts()) {
        if (item->backend() == backend || item->pendingBackend() == backend)
            return item;
    }
    return nullptr;
}

Backend* AccountManager::findAccount(const QString& server,
                                     const QString& userId) const
{
    const QString key = accountIdentityKey(server, userId);
    if (key.isEmpty())
        return nullptr;
    for (Account* item : allAccounts()) {
        const auto* record = item->record();
        if (record && record->hasConnectionIdentity()
            && key == accountIdentityKey(record->serverBaseUrl,
                                         record->serverUserId))
            return item->backend() && item->backend()->loggedIn()
                ? item->backend() : nullptr;
    }
    return nullptr;
}

Backend* AccountManager::createLoginBackend()
{
    return new Backend(this);
}

void AccountManager::discardCandidate(Backend* backend)
{
    if (!backend || backend->parent() != this)
        return;
    if (backend->loggedIn())
        backend->reset();
    else
        backend->cancelPendingLogin();
    backend->deleteLater();
}

Account* AccountManager::adoptAuthenticated(Backend* backend,
                                             const QString& loginName,
                                             const QString& displayName,
                                             const QString& recoveryId)
{
    if (!backend || !backend->loggedIn()
        || !backend->getStorage().loginUser)
        return nullptr;
    const QString server = backend->networkSession().host();
    const QString userId = backend->getStorage().loginUser->id;
    const QString key = accountIdentityKey(server, userId);
    if (key.isEmpty())
        return nullptr;

    WorkspaceLayout updated = _layout;
    QString id;
    if (!recoveryId.isEmpty()) {
        const auto* record = updated.account(recoveryId);
        if (!record)
            return nullptr;
        if (record->hasConnectionIdentity()) {
            if (accountIdentityKey(record->serverBaseUrl, record->serverUserId)
                != key)
                return nullptr;
            id = updated.upsertAuthenticatedAccount(
                server, userId, loginName, displayName);
            if (id != recoveryId)
                return nullptr;
        } else if (updated.resolveUnconfiguredAccount(
                       recoveryId, server, userId, loginName)) {
            id = recoveryId;
        }
    } else {
        id = updated.upsertAuthenticatedAccount(
            server, userId, loginName, displayName);
    }
    if (id.isEmpty())
        return nullptr;

    Account* existing = account(id);
    if (existing && existing->backend() == backend)
        return existing; // Re-registering the same live session is harmless.
    if (existing && (existing->backend()
                     || (existing->pendingBackend()
                         && existing->pendingBackend() != backend)))
        return nullptr;

    _layout = std::move(updated);
    Account* item = existing ? existing : ensureAccount(id);
    if (!item || !item->adoptBackend(backend))
        return nullptr;
    const bool firstAccount = _selectedId.isEmpty();
    if (firstAccount)
        _selectedId = id;
    rebuildModel();
    persist();
    if (firstAccount)
        emit activeBackendChanged(backend);
    emit accountRegistered(backend);
    emit accountConnected(item, backend);
    return item;
}

void AccountManager::rebuildModel()
{
    QVector<AccountListModel::Entry> entries;
    entries.reserve(_layout.accounts.size());
    for (const auto& record : _layout.accounts) {
        const Account* item = account(record.id);
        AccountConnectionState state = item
            ? item->state() : AccountConnectionState::Disconnected;
        if (!record.hasConnectionIdentity())
            state = AccountConnectionState::SetupRequired;
        entries.append({record.id, record.name,
                        QUrl(record.serverBaseUrl).host(), state});
    }
    _listModel.setEntries(entries, _selectedId);
}

void AccountManager::cancelPendingRestore(
    const QString& id, AccountConnectionState status)
{
    if (Account* item = account(id))
        item->cancelPending(status);
}

void AccountManager::restoreAll()
{
    for (Account* item : allAccounts()) {
        if (item->record() && item->record()->hasConnectionIdentity())
            restoreAccount(item->id());
    }
}

void AccountManager::restoreAccount(const QString& id)
{
    Account* item = account(id);
    const auto* record = item ? item->record() : nullptr;
    if (_invalidCatalog || !record || !record->hasConnectionIdentity()
        || item->backend() || item->pendingBackend())
        return;

    Backend* backend = item->createPendingBackend();
    if (!backend)
        return;
    QPointer<Account> accountGuard(item);
    QPointer<Backend> backendGuard(backend);
    const QString server = record->serverBaseUrl;
    const QString login = record->loginName;
    SessionTokenStore::read(item, server, login,
                            [this, accountGuard, backendGuard, server, login](
                                SessionTokenStore::ReadStatus status,
                                const QString& token) {
        Account* account = accountGuard.data();
        Backend* backend = backendGuard.data();
        if (!account || !backend || account->pendingBackend() != backend)
            return;
        if (status != SessionTokenStore::ReadStatus::Found) {
            account->cancelPending(
                status == SessionTokenStore::ReadStatus::NotFound
                    ? AccountConnectionState::AuthenticationRequired
                    : AccountConnectionState::Unavailable);
            return;
        }

        BackendLoginData credentials;
        credentials.domain = server;
        credentials.username = login;
        credentials.token = token;
        backend->login(credentials, [this, accountGuard, backendGuard](
                                            const QString& authenticatedToken) {
            Account* account = accountGuard.data();
            Backend* backend = backendGuard.data();
            if (!account || !backend || account->pendingBackend() != backend)
                return;
            if (authenticatedToken.isEmpty()) {
                const bool transient = backend->lastLoginFailureTransient();
                account->cancelPending(
                    transient ? AccountConnectionState::Unavailable
                              : AccountConnectionState::AuthenticationRequired);
                return;
            }
            const WorkspaceLayout::Account* record = account->record();
            if (!record || accountIdentityKey(
                    record->serverBaseUrl, record->serverUserId)
                    != accountIdentityKey(backend->networkSession().host(),
                                           backend->getLoginUser().id)) {
                qWarning("Saved credentials resolved to a different account");
                account->cancelPending(AccountConnectionState::AuthenticationRequired);
                return;
            }
            if (!adoptAuthenticated(backend, record->loginName,
                                    record->name, account->id())) {
                account->cancelPending(AccountConnectionState::AuthenticationRequired);
            }
        });
    });
}

void AccountManager::prepareStop(Account* item, bool authenticationExpired)
{
    if (!item || !item->backend())
        return;
    item->setState(authenticationExpired
        ? AccountConnectionState::AuthenticationRequired
        : AccountConnectionState::Disconnected);
    if (const auto* record = item->record())
        SessionTokenStore::remove(this, record->serverBaseUrl, record->loginName);
    item->backend()->prepareForLogout();
    if (_selectedId == item->id())
        emit activeBackendChanged(nullptr);
}

void AccountManager::finishBackendRemoval(Account* item, Backend* backend)
{
    if (!item || !backend || item->backend() != backend)
        return;
    emit accountUnregistered(backend);
    item->releaseBackend();
    if (item->id() == _selectedId)
        emit activeBackendChanged(nullptr);
    rebuildModel();
}

void AccountManager::finishStop(Account* item, bool authenticationExpired)
{
    if (!item || !item->backend())
        return;
    QPointer<Account> guard(item);
    QPointer<Backend> backend(item->backend());
    if (authenticationExpired) {
        backend->reset();
        QTimer::singleShot(0, this, [this, guard, backend] {
            finishBackendRemoval(guard, backend);
        });
    } else {
        backend->logout([this, guard, backend] {
            QTimer::singleShot(0, this, [this, guard, backend] {
                finishBackendRemoval(guard, backend);
            });
        });
    }
}

} // namespace Mattermost
