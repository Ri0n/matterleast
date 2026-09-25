/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace Mattermost {

struct DraftEntry {
    QString channelId;
    QString rootId;
    QString message;
    QString replyToPostId;
    // Local file paths are used only by recovered outbox drafts. Mattermost's
    // remote draft API has no attachment-intent representation.
    QStringList attachmentPaths;
    // Local-only identity for recovered unsent messages. Ordinary Mattermost
    // drafts keep this empty and retain the server-compatible (channel, root)
    // uniqueness rule.
    QString recoveryId;
    qint64 updateAt = 0;
    bool dirty = false;
    bool deleted = false;
    bool remotePresent = false;
    bool syncRequested = false;

    QString key() const;
    bool isRecovered() const { return !recoveryId.isEmpty(); }
};

class DraftStore final
{
public:
    static QVector<DraftEntry> load(const QString& path, bool* ok = nullptr);
    static bool save(const QString& path, const QVector<DraftEntry>& drafts);
};

} // namespace Mattermost
