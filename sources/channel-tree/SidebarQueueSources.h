#pragma once

#include <functional>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>

namespace Mattermost {

class Backend;
class BackendUser;

// A queue observes the existing per-account models. This object owns only
// subscriptions to visible sessions, not read cursors or copied model state.
class SidebarQueueSources final : public QObject
{
public:
    using Accounts = QMap<QString, Backend*>;
    using UserChanged = std::function<void(const QString&, const BackendUser&)>;

    SidebarQueueSources(QObject* parent, std::function<void()> changed,
                        UserChanged userChanged);
    ~SidebarQueueSources() override;

    static QString accountId(const Backend& backend);
    static QString key(const QString& accountId, const QString& destination);
    Backend* backend(const QString& accountId) const;
    Accounts accounts() const;
    void setAccounts(const Accounts& accounts);
    void observeUser(const QString& id, BackendUser& user);
    void refreshThreads();

private:
    struct Source {
        QPointer<Backend> backend;
        QObject* context = nullptr;
        QSet<const BackendUser*> users;
    };
    void remove(const QString& id);

    QMap<QString, Source> sources_;
    std::function<void()> changed_;
    UserChanged userChanged_;
};

} // namespace Mattermost
