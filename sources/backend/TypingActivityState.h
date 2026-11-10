#pragma once

#include <algorithm>
#include <limits>

#include <QHash>
#include <QString>
#include <QStringList>

namespace Mattermost {

// Transient, session-only typing state. A single caller-owned timer drives
// all conversations; this class never creates one timer per user or event.
class TypingActivityState
{
public:
    static QString contextKey(const QString& channelId, const QString& parentId)
    {
        return channelId + QLatin1Char('/') + parentId;
    }

    static QString channelFromKey(const QString& key)
    {
        return key.section(QLatin1Char('/'), 0, 0);
    }

    static QString parentFromKey(const QString& key)
    {
        return key.section(QLatin1Char('/'), 1);
    }

    bool observe(const QString& channelId, const QString& parentId,
                 const QString& userId, qint64 nowMs, qint64 lifetimeMs)
    {
        if (channelId.isEmpty() || userId.isEmpty()) {
            return false;
        }
        const QString key = contextKey(channelId, parentId);
        auto& users = _users[key];
        const bool added = !users.contains(userId);
        users.insert(userId, nowMs + std::max<qint64>(1000, lifetimeMs));
        return added;
    }

    bool remove(const QString& channelId, const QString& parentId,
                const QString& userId)
    {
        const QString key = contextKey(channelId, parentId);
        auto it = _users.find(key);
        if (it == _users.end() || !it->remove(userId)) {
            return false;
        }
        if (it->isEmpty()) {
            _users.erase(it);
        }
        return true;
    }

    QStringList users(const QString& channelId, const QString& parentId) const
    {
        QStringList result = _users.value(contextKey(channelId, parentId)).keys();
        result.sort();
        return result;
    }

    // Returns only contexts whose visible user set changed.
    QStringList expire(qint64 nowMs)
    {
        QStringList changed;
        for (auto it = _users.begin(); it != _users.end();) {
            bool removed = false;
            for (auto user = it->begin(); user != it->end();) {
                if (user.value() <= nowMs) {
                    user = it->erase(user);
                    removed = true;
                } else {
                    ++user;
                }
            }
            if (removed) {
                changed.append(it.key());
            }
            if (it->isEmpty()) {
                it = _users.erase(it);
            } else {
                ++it;
            }
        }
        return changed;
    }

    qint64 nextExpiryMs() const
    {
        qint64 result = std::numeric_limits<qint64>::max();
        for (auto it = _users.cbegin(); it != _users.cend(); ++it) {
            for (auto user = it.value().cbegin(); user != it.value().cend(); ++user) {
                result = std::min(result, user.value());
            }
        }
        return result;
    }

    bool clearContext(const QString& channelId, const QString& parentId)
    {
        return static_cast<bool>(_users.remove(contextKey(channelId, parentId)));
    }

    QStringList clear()
    {
        const QStringList changed = _users.keys();
        _users.clear();
        return changed;
    }

private:
    QHash<QString, QHash<QString, qint64>> _users;
};

} // namespace Mattermost
