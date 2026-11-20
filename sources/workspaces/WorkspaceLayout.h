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
        QString id;
        QString name;
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

    // Versioned JSON without tokens or passwords.
    QByteArray toJson() const;
    static std::optional<WorkspaceLayout> fromJson(const QByteArray& json,
                                                    QString* error = nullptr);

private:
    const Profile* findProfile(const QString& id) const;
    const View* findView(const QString& id) const;
};

} // namespace Mattermost
