/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QHash>
#include <QJsonObject>
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
    bool findDraftByKey(const QString& key, DraftEntry& draft);

    // Debounced composer autosave is local-only.
    void updateDraft(const QString& channelId, const QString& rootId,
                     const QString& message, const QString& replyToPostId);
    // Explicitly publish the latest ordinary draft state.
    void flushDraft(const QString& channelId, const QString& rootId);
    // Explicit discard/send: delete remotely only when a server copy exists or
    // a previous remote flush is still outstanding.
    void removeDraft(const QString& channelId, const QString& rootId);

    // Recovered unsent messages are intentionally local-only. Multiple entries
    // may coexist for one conversation, keyed by their former pending_post_id.
    QString addRecoveredDraft(const QString& channelId,
                              const QString& rootId,
                              const QString& message,
                              const QString& replyToPostId,
                              const QString& recoveryId);
    void updateRecoveredDraft(const QString& key,
                              const QString& message,
                              const QString& replyToPostId);
    void removeDraftByKey(const QString& key);

    void syncAllTeams();

    // Mattermost emits draft_created/draft_updated/draft_deleted over the
    // authenticated WebSocket. Merge those notifications with the same
    // timestamp/conflict semantics as HTTP draft synchronization.
    void applyRemoteDraftEvent(const QJsonObject& data, bool deleted);

signals:
    void draftsChanged();
    void draftChanged(const QString& channelId, const QString& rootId);

private:
    explicit DraftService(Backend& backend);

    void ensureIdentity();
    bool persist();
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
