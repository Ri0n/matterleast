/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QString>
#include <QVector>

namespace Mattermost {

struct DraftEntry {
    QString channelId;
    QString rootId;
    QString message;
    QString replyToPostId;
    qint64 updateAt = 0;
    bool dirty = false;
    bool deleted = false;

    QString key() const;
};

class DraftStore final
{
public:
    static QVector<DraftEntry> load(const QString& path, bool* ok = nullptr);
    static bool save(const QString& path, const QVector<DraftEntry>& drafts);
};

} // namespace Mattermost
