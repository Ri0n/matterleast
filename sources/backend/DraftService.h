/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "DraftStore.h"
#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

class DraftService final : public QObject
{
    Q_OBJECT
public:
    static DraftService& instance(Backend& backend);

    QVector<DraftEntry> drafts();
    bool findDraft(const QString& channelId, const QString& rootId,
                   DraftEntry& draft);

    void updateDraft(const QString& channelId, const QString& rootId,
                     const QString& message, const QString& replyToPostId);
    void removeDraft(const QString& channelId, const QString& rootId);
    void syncAllTeams();

signals:
    void draftsChanged();
    void draftChanged(const QString& channelId, const QString& rootId);

private:
    explicit DraftService(Backend& backend);

    void ensureIdentity();
    void persist();
    void syncEntry(const DraftEntry& draft);
    void finishTeamSync(quint64 generation);
    void disableRemoteSync();

    Backend& backend;
    HTTPConnector connector;
    QHash<QString, DraftEntry> entries;
    QString identity;
    QString storePath;
    bool remoteSyncDisabled = false;
    quint64 syncGeneration = 0;
    int pendingTeamSyncs = 0;
    bool teamSyncHadError = false;
    QSet<QString> remoteKeysSeen;
};

} // namespace Mattermost
