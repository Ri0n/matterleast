#pragma once

#include <functional>
#include <utility>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>

#include "backend/Account.h"
#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/UserProfileService.h"
#include "ui/UserStatusEmoji.h"

namespace Mattermost {

// A queue observes the existing per-account models. This object owns only
// subscriptions to visible sessions, not read cursors or copied model state.
class SidebarQueueSources final : public QObject
{
public:
    using Accounts = QMap<QString, Backend*>;
    using UserChanged = std::function<void(const QString&, const BackendUser&)>;

    SidebarQueueSources(QObject* parent, std::function<void()> changed,
                        UserChanged userChanged)
        : QObject(parent), changed_(std::move(changed)),
          userChanged_(std::move(userChanged)) {}

    static QString accountId(const Backend& backend)
    {
        const auto* account = qobject_cast<const Account*>(backend.parent());
        return account ? account->id() : QString();
    }

    static QString key(const QString& accountId, const QString& destination)
    {
        return accountId + QChar(0x1f) + destination;
    }

    Backend* backend(const QString& accountId) const
    {
        auto it = sources_.constFind(accountId);
        return it == sources_.cend() ? nullptr : it->backend.data();
    }

    Accounts accounts() const
    {
        Accounts result;
        for (auto it = sources_.cbegin(); it != sources_.cend(); ++it)
            if (it->backend) result.insert(it.key(), it->backend.data());
        return result;
    }

    void setAccounts(const Accounts& accounts)
    {
        for (const QString& id : sources_.keys()) {
            if (backend(id) != accounts.value(id) || !backend(id))
                remove(id);
        }
        for (auto it = accounts.cbegin(); it != accounts.cend(); ++it) {
            if (!it.value() || backend(it.key()) == it.value()) continue;
            const QString id = it.key();
            Backend* owner = it.value();
            Source source;
            source.backend = owner;
            source.context = new QObject(this);
            sources_.insert(id, source);
            auto* context = source.context;
            const QPointer<Backend> guard(owner);
            connect(owner, &QObject::destroyed, context, [this, id] {
                remove(id);
                changed_();
            });
            auto& model = FollowingModel::instance(*owner);
            connect(&model, &FollowingModel::changed, context, [this] { changed_(); });
            connect(&UserProfileService::instance(*owner), &UserProfileService::profileResolved,
                    context, [this, guard, id](const QString& userId) {
                if (!guard) return;
                if (auto* user = guard->getStorage().getUserById(userId))
                    userChanged_(id, *user);
            });
            connect(&owner->emojiRegistry(), &EmojiRegistry::customEmojiAdded,
                    context, [this, guard, id](const QString& name) {
                if (!guard) return;
                const auto users = sources_.value(id).users;
                for (const BackendUser* user : users)
                    if (UserStatusEmoji::name(*user) == name)
                        userChanged_(id, *user);
            });
            model.ensureThreadsFresh();
        }
    }

    void observeUser(const QString& id, BackendUser& user)
    {
        auto it = sources_.find(id);
        if (it == sources_.end() || it->users.contains(&user)) return;
        it->users.insert(&user);
        QObject* context = it->context;
        auto notify = [this, id, &user] { userChanged_(id, user); };
        connect(&user, &BackendUser::onStatusChanged, context, notify);
        connect(&user, &BackendUser::onAvatarChanged, context, notify);
        connect(&user, &BackendUser::onCustomStatusChanged, context, notify);
        connect(&user, &QObject::destroyed, context, [this, id, key = &user] {
            auto it = sources_.find(id);
            if (it != sources_.end()) it->users.remove(key);
        });
    }

    void refreshThreads()
    {
        for (auto* owner : accounts())
            FollowingModel::instance(*owner).ensureThreadsFresh();
    }

private:
    struct Source {
        QPointer<Backend> backend;
        QObject* context = nullptr;
        QSet<const BackendUser*> users;
    };
    void remove(const QString& id)
    {
        auto source = sources_.take(id);
        delete source.context;
    }

    QMap<QString, Source> sources_;
    std::function<void()> changed_;
    UserChanged userChanged_;
};

} // namespace Mattermost
