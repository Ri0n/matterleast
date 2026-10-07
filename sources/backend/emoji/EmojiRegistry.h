#pragma once

#include <optional>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "EmojiDefs.h"

namespace Mattermost {

/**
 * Backend-scoped emoji presentation registry.
 *
 * Generated built-ins remain immutable in EmojiInfo. Runtime custom emoji are
 * stored here by Mattermost name so their lifetime follows the backend/session
 * and no process-global custom state can leak across servers.
 */
class EmojiRegistry final : public QObject
{
    Q_OBJECT
public:
    explicit EmojiRegistry(QObject* parent = nullptr);

    std::optional<Emoji> resolveByName(const QString& emojiName);
    QVector<Emoji> getAllEmojis(uint32_t category, uint32_t skinTone) const;

    void addCustomEmoji(const QString& emojiName, const QString& emojiPath);
    bool isCustomEmojiPath(const QString& emojiPath) const;

signals:
    void customEmojiRequested(const QString& name);
    void customEmojiAdded(const QString& name);

private:
    static bool isValidCustomEmojiName(const QString& name);
    static QString normalizedCustomEmojiPath(QString path);
    static Emoji customEmojiPresentation(const QString& name,
                                         const QString& path);

    QHash<QString, QString> _customEmojiPathsByName;
    QSet<QString> _customEmojiPaths;
};

} // namespace Mattermost
