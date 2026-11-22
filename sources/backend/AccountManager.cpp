#include "AccountManager.h"

#include <QLoggingCategory>
#include <QTimer>
#include <QUrl>
#include <utility>

#include "Account.h"
#include "backend/AccountIdentity.h"
#include "backend/Backend.h"
#include "backend/SessionTokenStore.h"
#include "backend/SessionVerificationRetry.h"
#include "backend/types/BackendLoginData.h"
#include "backend/types/BackendUser.h"
#include "navigation/ViewNavigationSession.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {
Q_LOGGING_CATEGORY(accountRestoreLog, "mattermost.account.restore")
}

AccountManager::AccountManager(QObject* parent)
    : QObject(parent)
    , _treeModel(_layout, _listModel, this)
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
    _treeModel.refreshTopology();
    ensureViewSessions();
    qCInfo(accountRestoreLog) << "Account catalog loaded:"
                              << _layout.accounts.size() << "accounts,"
                              << _layout.profiles.size() << "profiles,"
                              << _layout.views.size() << "views";
    if (error)
        error->clear();
    return true;
}

bool AccountManager::loadFromOptions(QString* error)
{
    const QByteArray json = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("workspaces/layout"));
    if (!load(json, error)) {
        qCWarning(accountRestoreLog) << "Saved account catalog is invalid";
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

bool AccountManager::applyLayoutEdits(const WorkspaceLayout& proposed,
                                      QString* error)
{
    auto reject = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (_invalidCatalog || _layout.accounts.isEmpty())
        return reject(QStringLiteral("Workspace catalog is unavailable"));
    if (!proposed.validate(error)) return false;
    if (proposed.accounts.size() != _layout.accounts.size())
        return reject(QStringLiteral("Adding or removing authenticated accounts requires the account lifecycle API"));
    // Never change a verified remote identity from a layout editor. Tokens
    // remain in the keyring; a different server/login must be added or
    // authenticated explicitly through the credential workflow.
    for (const auto& oldAccount : _layout.accounts) {
        const auto* updated = proposed.account(oldAccount.id);
        if (!updated || updated->serverBaseUrl != oldAccount.serverBaseUrl
            || updated->serverUserId != oldAccount.serverUserId
            || updated->loginName != oldAccount.loginName)
            return reject(QStringLiteral("Server identity cannot be edited without authentication"));
    }
    const QString oldViewId = _layout.activeViewId;
    const QString oldSelectedId = _selectedId;
    const QStringList oldProfileAccounts = activeProfileAccountIds();
    const QSet<QString> oldProfileIds(oldProfileAccounts.cbegin(),
                                     oldProfileAccounts.cend());
    _layout = proposed;
    QStringList activeIds;
    for (const auto& view : _layout.views)
        if (view.id == _layout.activeViewId) {
            activeIds = view.accountIds;
            break;
        }
    if (!activeIds.contains(_selectedId))
        _selectedId = activeIds.first();
    _lastSelectedByView.insert(_layout.activeViewId, _selectedId);
    for (auto it = _lastSelectedByView.begin(); it != _lastSelectedByView.end();) {
        bool found = false;
        for (const auto& view : _layout.views)
            if (view.id == it.key()) { found = true; break; }
        if (found) ++it;
        else it = _lastSelectedByView.erase(it);
    }
    for (auto it = _viewSessions.begin(); it != _viewSessions.end();) {
        bool found = false;
        for (const auto& view : _layout.views)
            if (view.id == it.key()) { found = true; break; }
        if (found) ++it;
        else {
            it.value()->deleteLater();
            it = _viewSessions.erase(it);
        }
    }
    ensureViewSessions();
    rebuildModel();
    _treeModel.refreshTopology();
    persist();
    emit layoutChanged();
    if (oldViewId != _layout.activeViewId) {
        emit activeViewChanged(_layout.activeViewId);
        emit activeViewSessionChanged();
    }
    if (oldSelectedId != _selectedId)
        emit activeBackendChanged(activeBackend());
    const QStringList nextProfileAccounts = activeProfileAccountIds();
    const QSet<QString> newProfileIds(nextProfileAccounts.cbegin(),
                                     nextProfileAccounts.cend());
    if (oldProfileIds != newProfileIds)
        reconcileActiveProfile();
    if (error) error->clear();
    return true;
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

ViewNavigationSession* AccountManager::viewSession(const QString& viewId) const
{
    return _viewSessions.value(viewId, nullptr);
}

QAbstractItemModel* AccountManager::activeViewTabs() const
{
    return viewSession(_layout.activeViewId);
}

void AccountManager::ensureViewSessions()
{
    for (const auto& view : _layout.views) {
        if (!_viewSessions.contains(view.id))
            _viewSessions.insert(view.id, new ViewNavigationSession(view.id, this));
        // Membership edits are domain-level security/identity changes, not
        // merely a filtered sidebar. Retired destinations must disappear from
        // the view's tabs, dock and Back/Forward before Widgets can rebind.
        _viewSessions.value(view.id)->setAllowedAccounts(view.accountIds);
    }
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
    if (!account(id))
        return false;
    // Keep the existing preference rule: first look in the current view,
    // then in another view of this profile, then in another profile.
    WorkspaceLayout requested = _layout;
    if (!requested.selectAccount(id))
        return false;
    return applySelection(requested.activeViewId, id);
}

bool AccountManager::activateView(const QString& viewId)
{
    return applySelection(viewId, {});
}

bool AccountManager::applySelection(const QString& viewId, const QString& requestedId)
{
    const auto transition = _layout.transitionTo(viewId);
    if (!transition || transition->visibleAccountIds.isEmpty())
        return false;

    const bool viewChanged = _layout.activeViewId != viewId;
    QString oldProfileId;
    if (viewChanged) {
        for (const auto& view : _layout.views) {
            if (view.id == _layout.activeViewId) {
                oldProfileId = view.profileId;
                break;
            }
        }
    }
    const bool profileChanged = viewChanged
        && oldProfileId != transition->targetProfileId;

    QString selected = requestedId;
    if (selected.isEmpty() || !transition->visibleAccountIds.contains(selected)) {
        const QString previous = _lastSelectedByView.value(viewId);
        if (!previous.isEmpty() && transition->visibleAccountIds.contains(previous))
            selected = previous;
        else if (transition->visibleAccountIds.contains(_selectedId))
            selected = _selectedId;
        else
            selected = transition->visibleAccountIds.first();
    }

    if (!viewChanged && _selectedId == selected)
        return true; // No UI, HTTP or model churn on repeated selection.

    if (viewChanged && !_selectedId.isEmpty())
        _lastSelectedByView.insert(_layout.activeViewId, _selectedId);
    if (viewChanged && !_layout.selectView(viewId))
        return false;
    const bool accountChanged = _selectedId != selected;
    _selectedId = selected;
    _lastSelectedByView.insert(viewId, selected);
    _treeModel.syncActiveView();
    rebuildModel();
    // Only the active *view* is persisted. Remembering a different account
    // inside the same view is process-local and requires no settings I/O.
    if (viewChanged)
        persist();

    if (viewChanged) {
        emit activeViewChanged(viewId);
        emit activeViewSessionChanged();
    }
    if (accountChanged)
        emit activeBackendChanged(activeBackend());

    if (profileChanged)
        reconcileActiveProfile();
    return true;
}

QStringList AccountManager::activeProfileAccountIds() const
{
    for (const auto& view : _layout.views) {
        if (view.id != _layout.activeViewId)
            continue;
        for (const auto& profile : _layout.profiles) {
            if (profile.id == view.profileId)
                return profile.accountIds;
        }
        break;
    }
    return {};
}

void AccountManager::restoreActiveProfile()
{
    qCInfo(accountRestoreLog) << "Restoring active profile";
    reconcileActiveProfile();
}

void AccountManager::reconcileActiveProfile()
{
    const QStringList admittedIds = activeProfileAccountIds();
    const QSet<QString> admitted(admittedIds.cbegin(), admittedIds.cend());
    qCInfo(accountRestoreLog) << "Reconciling profile:"
                              << admittedIds.size() << "admitted accounts";

    // First mark EVERY outgoing connection for suspension, before emitting
    // any signals: the presentation may synchronously acknowledge one of
    // them. Otherwise a nested reconcile could start incoming accounts while
    // a second outgoing Backend is still alive.
    QVector<QPair<Account*, Backend*>> outgoing;
    const auto accounts = allAccounts();
    for (Account* item : accounts) {
        if (admitted.contains(item->id()))
            continue;
        if (Backend* backend = item->backend()) {
            if (_suspendingProfileAccounts.contains(item->id()))
                continue;
            _suspendingProfileAccounts.insert(item->id());
            outgoing.append({item, backend});
        }
    }

    // setState(), cancelPending() and prepareForLogout() can notify other
    // objects synchronously. Every outgoing Backend is already fenced by
    // _suspendingProfileAccounts before any of those callbacks are delivered.
    for (Account* item : accounts) {
        if (!admitted.contains(item->id()) && item->pendingBackend())
            item->cancelPending(AccountConnectionState::Disconnected);
    }
    for (const auto& entry : outgoing) {
        entry.first->setState(AccountConnectionState::Disconnected);
        entry.second->prepareForLogout();
    }
    for (const auto& entry : outgoing)
        emit profileSuspensionRequested(entry.first, entry.second);

    // A profile is exclusive. Its new sessions may start only AFTER the
    // outgoing backend-dependent Widgets have been destroyed and acknowledged.
    if (!_suspendingProfileAccounts.isEmpty())
        return;

    for (const QString& id : admittedIds) {
        Account* item = account(id);
        if (item && item->state() == AccountConnectionState::Disconnected
            && !item->backend() && !item->pendingBackend())
            restoreAccount(id);
    }
}

void AccountManager::finishProfileSuspension(Account* item, Backend* backend)
{
    if (!item || !backend || !_suspendingProfileAccounts.contains(item->id())
        || item->backend() != backend)
        return;

    // The application has already destroyed the dependent Widgets. Cancel
    // outstanding HTTP/WS work while the Backend still owns its Storage.
    backend->reset();
    emit accountUnregistered(backend);
    item->releaseBackend();
    _suspendingProfileAccounts.remove(item->id());
    rebuildModel();
    reconcileActiveProfile();
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
                                             QString loginName,
                                             QString displayName,
                                             QString recoveryId,
                                             bool persistToken)
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

    const auto* previous = _layout.account(id);
    const QString previousServer = previous ? previous->serverBaseUrl : QString();
    const QString previousLogin = previous ? previous->loginName : QString();
    const QString previousViewId = _layout.activeViewId;
    _layout = std::move(updated);
    Account* item = existing ? existing : ensureAccount(id);
    if (!item || !item->adoptBackend(backend))
        return nullptr;
    const bool firstAccount = _selectedId.isEmpty();
    if (firstAccount)
        _selectedId = id;
    rebuildModel();
    _treeModel.refreshTopology();
    ensureViewSessions();
    persist();
    if (previousViewId != _layout.activeViewId) {
        emit activeViewChanged(_layout.activeViewId);
        emit activeViewSessionChanged();
    }
    if (persistToken) {
        // Only after the verified remote identity has been adopted may a
        // credential be written to secure storage. Restoring an existing
        // token does not write it again on every application startup.
        const QString authenticatedToken = backend->networkSession().token();
        if (!authenticatedToken.isEmpty()) {
            const QString acceptedServer = backend->networkSession().host();
            SessionTokenStore::write(item, acceptedServer, loginName,
                                     authenticatedToken,
                [this, itemGuard = QPointer<Account>(item),
                 previousServer, previousLogin, acceptedServer, loginName]
                (bool success) {
                if (!success || !itemGuard)
                    return;
                emit credentialsStored(acceptedServer, loginName);
                if (!previousLogin.isEmpty()
                    && (previousServer != acceptedServer
                        || previousLogin != loginName))
                    SessionTokenStore::remove(itemGuard.data(),
                                             previousServer, previousLogin);
            });
        }
    }
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

void AccountManager::restoreAccount(const QString& id)
{
    Account* item = account(id);
    const auto* record = item ? item->record() : nullptr;
    // A direct Retry action must not bypass exclusive profile policy.
    // The caller first activates a view admitting the requested account.
    if (_invalidCatalog || !record || !record->hasConnectionIdentity()
        || !activeProfileAccountIds().contains(id)
        || !_suspendingProfileAccounts.isEmpty()
        || item->backend() || item->pendingBackend())
        return;

    qCInfo(accountRestoreLog) << "Starting session restoration for account" << id;
    Backend* backend = item->createPendingBackend();
    if (!backend) {
        qCWarning(accountRestoreLog) << "Could not create pending backend for account" << id;
        return;
    }
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
        qCInfo(accountRestoreLog) << "Saved session keyring lookup for account"
                                  << account->id()
                                  << (status == SessionTokenStore::ReadStatus::Found ? "found"
                                      : status == SessionTokenStore::ReadStatus::NotFound
                                          ? "not-found" : "unavailable");
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
        qCInfo(accountRestoreLog) << "Verifying saved session for account" << account->id();
        // The token is already in the keyring. A DNS failure or HTTP 5xx
        // cannot invalidate it and must not force interactive sign-in.
        // The verifier is owned by the pending Backend: cancelling a restore
        // destroys its delayed retries together with that Backend.
        auto* verifier = new SessionVerificationRetry(backend);
        verifier->start(
            [backendGuard, credentials](SessionVerificationRetry::CheckDone done) {
                if (!backendGuard) return;
                backendGuard->login(credentials,
                    [backendGuard, done = std::move(done)](const QString& token) {
                    if (!backendGuard) return;
                    const bool authenticated = !token.isEmpty();
                    done(authenticated, !authenticated
                        && backendGuard->lastLoginFailureTransient());
                });
            },
            [this, accountGuard, backendGuard](bool authenticated, bool transient) {
                Account* account = accountGuard.data();
                Backend* backend = backendGuard.data();
                if (!account || !backend || account->pendingBackend() != backend)
                    return;
                if (!authenticated) {
                    qCWarning(accountRestoreLog)
                        << "Saved session verification did not succeed for account"
                        << account->id() << "transient" << transient;
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
                    qCWarning(accountRestoreLog)
                        << "Saved credentials resolved to a different remote identity for account"
                        << account->id();
                    account->cancelPending(AccountConnectionState::AuthenticationRequired);
                    return;
                }
                if (!adoptAuthenticated(backend, record->loginName,
                                        record->name, account->id(), false)) {
                    qCWarning(accountRestoreLog)
                        << "Verified session could not be adopted for account"
                        << account->id();
                    account->cancelPending(AccountConnectionState::AuthenticationRequired);
                } else {
                    qCInfo(accountRestoreLog) << "Session restored for account" << account->id();
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
    if (_suspendingProfileAccounts.remove(item->id()))
        reconcileActiveProfile();
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
