#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include "workspaces/AccountListModel.h"
#include "workspaces/WorkspaceTreeModel.h"
#include "workspaces/WorkspaceLayout.h"

namespace Mattermost {
class Account;
class Backend;
class ViewNavigationSession;

// Single owner of durable account QObjects and optional session Backends.
// WorkspaceLayout remains pure serializable configuration; Widgets and QML
// consume listModel() and signals, never mutate connection maps themselves.
class AccountManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* workspaceTreeModel READ workspaceTreeModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* activeViewTabs READ activeViewTabs
               NOTIFY activeViewSessionChanged)
public:
    explicit AccountManager(QObject* parent = nullptr);

    bool load(const QByteArray& json, QString* error = nullptr);
    bool loadFromOptions(QString* error = nullptr);
    bool empty() const { return _layout.accounts.isEmpty(); }
    bool invalidCatalog() const { return _invalidCatalog; }
    const WorkspaceLayout& layout() const { return _layout; }
    AccountListModel& listModel() { return _listModel; }
    const AccountListModel& listModel() const { return _listModel; }
    QAbstractItemModel* workspaceTreeModel() { return &_treeModel; }
    // View session contains only semantic tab/history state, not duplicated
    // Backend or chat widgets. Views may share accounts freely.
    ViewNavigationSession* viewSession(const QString& viewId) const;
    QAbstractItemModel* activeViewTabs() const;

    Account* account(const QString& id) const { return _byId.value(id, nullptr); }
    Account* accountForBackend(const Backend* backend) const;
    QVector<Account*> allAccounts() const;
    QVector<Backend*> registeredBackends() const;

    QString selectedAccountId() const { return _selectedId; }
    Backend* activeBackend() const;
    bool selectAccount(const QString& id);
    // Activate a named view. Only a profile change can alter connection
    // lifetimes: switching views within a profile leaves every session alive.
    bool activateView(const QString& viewId);
    QString activeViewId() const { return _layout.activeViewId; }
    void persist() const;
    // Commit edited profile/view topology atomically. Connection identity and
    // credentials are immutable here; account labels may be changed. All
    // network lifetime reconciliation remains owned by AccountManager.
    bool applyLayoutEdits(const WorkspaceLayout& proposed, QString* error = nullptr);

    // Temporary candidates for user-entered credentials belong to this
    // manager until successfully reparented to their durable Account.
    Backend* createLoginBackend();
    void discardCandidate(Backend* candidate);
    // Own all arguments before replacing _layout. Restoring existing accounts
    // passes QStrings referring to the old layout's Account records.
    Account* adoptAuthenticated(Backend* candidate, QString loginName,
                                QString displayName,
                                QString recoveryId = {},
                                bool persistToken = true);
    Backend* findAccount(const QString& server, const QString& userId) const;

    void restoreAccount(const QString& id);
    // Starts only accounts admitted by the active profile, including hidden
    // accounts in its other views. Failed/auth-required restores need an
    // explicit retry through restoreAccount().
    void restoreActiveProfile();
    void cancelPendingRestore(const QString& id, AccountConnectionState status);

    // UI releases its widget tree between prepareStop() and finishStop().
    // This ordering avoids dangling references to Backend-owned Storage.
    void prepareStop(Account* account, bool authenticationExpired);
    void finishStop(Account* account, bool authenticationExpired);
    // Called after the presentation has disposed of every widget that
    // references the old Backend's Storage. Profile suspension is NOT logout:
    // no /users/logout request and no keychain deletion.
    void finishProfileSuspension(Account* account, Backend* backend);

signals:
    void accountRegistered(Backend* backend);
    void accountUnregistered(Backend* backend);
    void activeBackendChanged(Backend* backend);
    void accountConnected(Account* account, Backend* backend);
    void activeViewChanged(const QString& viewId);
    void activeViewSessionChanged();
    void layoutChanged();
    // Presentation must release Backend-dependent widgets first and then
    // acknowledge with finishProfileSuspension. Other profiles cannot connect
    // before all outgoing sessions are released.
    void profileSuspensionRequested(Account* account, Backend* backend);
    // Emitted only after a verified account's token is safely stored in
    // platform keychain. The migration layer may then drop old plaintext.
    void credentialsStored(const QString& server, const QString& loginName);

private:
    Account* ensureAccount(const QString& id);
    void rebuildModel();
    void finishBackendRemoval(Account* account, Backend* backend);
    bool applySelection(const QString& viewId, const QString& accountId);
    void reconcileActiveProfile();
    void ensureViewSessions();
    QStringList activeProfileAccountIds() const;

    WorkspaceLayout _layout;
    QHash<QString, Account*> _byId;
    AccountListModel _listModel;
    WorkspaceTreeModel _treeModel;
    QHash<QString, ViewNavigationSession*> _viewSessions;
    QString _selectedId;
    QHash<QString, QString> _lastSelectedByView;
    QSet<QString> _suspendingProfileAccounts;
    bool _invalidCatalog = false;
};

} // namespace Mattermost
