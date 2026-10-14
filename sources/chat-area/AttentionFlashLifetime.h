#pragma once

#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QString>

namespace Mattermost {

/**
 * Tracks in-flight Attention animations without tying row-destruction
 * callbacks to the lifetime of ChatLogWidget's ordinary C++ members.
 *
 * ChatLogWidget must destroy this context in its own destructor, before the
 * LongListWidget/QWidget base classes destroy their materialized child rows.
 * Qt then disconnects each row's destroyed() handler before it can run during
 * that later base-class teardown. Replacing the context on source changes also
 * invalidates completion callbacks from the previous timeline.
 */
class AttentionFlashLifetime final : public QObject
{
public:
    bool isFlashing(const QString& postId) const
    {
        return _destroyedConnections.contains(postId);
    }

    bool begin(QObject& postWidget, const QString& postId)
    {
        if (postId.isEmpty() || isFlashing(postId)) {
            return false;
        }

        const auto connection = connect(&postWidget, &QObject::destroyed,
                                        this, [this, postId] {
            // Virtualized posts can disappear before their animation finishes.
            _destroyedConnections.remove(postId);
        });
        _destroyedConnections.insert(postId, connection);
        return true;
    }

    void finish(const QString& postId)
    {
        const auto it = _destroyedConnections.find(postId);
        if (it == _destroyedConnections.end()) {
            return;
        }
        // A completed animation must not accumulate destroyed() connections
        // when the same visible post is highlighted again later.
        disconnect(it.value());
        _destroyedConnections.erase(it);
    }

private:
    QHash<QString, QMetaObject::Connection> _destroyedConnections;
};

} // namespace Mattermost
