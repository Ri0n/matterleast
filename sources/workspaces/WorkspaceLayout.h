#pragma once

#include <optional>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Mattermost {

// Pure persisted workspace topology. No Backend, widgets or credentials.
// The same account ID can be used by multiple profiles and multiple views.
class WorkspaceLayout final
{
public:
    struct Account {
        QString id;             // Stable local identity across restarts.
        QString name;           // Display label, not a network identity.
        QString serverBaseUrl = {};  // Non-secret origin including installation path.
        QString serverUserId = {};   // Server-assigned account ID.
        QString loginName = {};      // Username/email for keyring lookup.

        // Imported v1 accounts have no connection metadata yet.
        bool hasConnectionIdentity() const
        {
            return !serverBaseUrl.isEmpty() && !serverUserId.isEmpty()
                && !loginName.isEmpty();
        }
    };
    struct Profile {
        QString id;
        QString name;
        QStringList accountIds; // All sessions kept connected.
    };
    struct View {
        QString id;
        QString profileId;
        QString name;
        QStringList accountIds; // Visible subset of profile accounts.
    };
    struct Transition {
        QString targetProfileId;
        QString targetViewId;
        QStringList disconnectAccountIds;
        QStringList connectAccountIds;
        QStringList visibleAccountIds;
    };

    QVector<Account> accounts;
    QVector<Profile> profiles;
    QVector<View> views;
    QString activeViewId;

    bool validate(QString* error = nullptr) const;

    // A plan is declarative: the caller disconnects before connecting and
    // reports transport errors separately. This method never changes state.
    std::optional<Transition> transitionTo(const QString& viewId) const;

    // Select the desired view only; does not start/stop any connection.
    bool selectView(const QString& viewId);

    // Pure, transactional catalog operations. They create no network clients,
    // widgets or account managers; the runtime AccountManager owns that state.
    const Account* account(const QString& accountId) const;
    QString upsertAuthenticatedAccount(const QString& serverBaseUrl,
                                       const QString& serverUserId,
                                       const QString& loginName,
                                       const QString& displayName);
    bool resolveUnconfiguredAccount(const QString& accountId,
                                    const QString& serverBaseUrl,
                                    const QString& serverUserId,
                                    const QString& loginName);
    bool selectAccount(const QString& accountId);

    // Version 2 adds non-secret remote identity; version 1 remains readable.
    // Tokens and passwords never enter the layout.
    QByteArray toJson() const;
    static std::optional<WorkspaceLayout> fromJson(const QByteArray& json,
                                                    QString* error = nullptr);

private:
    const Profile* findProfile(const QString& id) const;
    const View* findView(const QString& id) const;
};

} // namespace Mattermost
