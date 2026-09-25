/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "DraftService.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>

#include "Backend.h"
#include "NetworkRequest.h"
#include "PostProps.h"
#include "QByteArrayCreator.h"
#include "Storage.h"
#include "types/BackendTeam.h"
#include "types/BackendUser.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcDraftTrace, "mattermost.draft", QtInfoMsg)

QString entryKey(const QString& channelId, const QString& rootId)
{
    return channelId + QChar(0x1f) + rootId;
}

qint64 nextUpdateTime(const DraftEntry* previous)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return previous ? std::max(now, previous->updateAt + 1) : now;
}

int httpStatus(const QNetworkReply& reply)
{
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool httpSucceeded(const QNetworkReply& reply)
{
    const int status = httpStatus(reply);
    return reply.error() == QNetworkReply::NoError
        && status >= 200 && status < 300;
}

bool draftApiUnsupported(const QNetworkReply& reply)
{
    const int status = httpStatus(reply);
    return status == 404 || status == 405 || status == 501;
}

QJsonObject remoteDraftObject(const QJsonObject& data)
{
    const QJsonValue value = data.value(QStringLiteral("draft"));
    if (value.isObject()) {
        return value.toObject();
    }
    if (!value.isString()) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

DraftEntry remoteDraft(const QJsonObject& object)
{
    DraftEntry entry;
    entry.channelId = object.value(QStringLiteral("channel_id")).toString();
    entry.rootId = object.value(QStringLiteral("root_id")).toString();
    entry.message = object.value(QStringLiteral("message")).toString();
    entry.updateAt =
        object.value(QStringLiteral("update_at")).toVariant().toLongLong();

    const QJsonObject props = object.value(QStringLiteral("props")).toObject();
    entry.replyToPostId =
        props.value(QString::fromLatin1(PostProps::ReplyToPostId)).toString();
    return entry;
}

} // namespace

DraftService& DraftService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<DraftService>> instances;
    QPointer<DraftService>& service = instances[&backend];
    if (!service) {
        service = new DraftService(backend);
    }
    return *service;
}

DraftService::DraftService(Backend& backendInstance)
    : QObject(&backendInstance)
    , backend(backendInstance)
{
    ensureIdentity();

    connect(&backend, &Backend::onAllTeamChannelsPopulated,
            this, &DraftService::syncAllTeams);
    connect(&backend, &Backend::onWebSocketConnect,
            this, [this] {
        if (!backend.getStorage().teams.empty()) {
            syncAllTeams();
        }
    });
}

void DraftService::ensureIdentity()
{
    const QString userId = backend.getLoginUser().id;
    if (userId.isEmpty()) {
        return;
    }

    QByteArray identityMaterial = NetworkRequest::host().toUtf8();
    identityMaterial.append('\0');
    identityMaterial.append(userId.toUtf8());
    const QString nextIdentity = QString::fromLatin1(
        QCryptographicHash::hash(identityMaterial, QCryptographicHash::Sha256)
            .toHex());

    if (identity == nextIdentity) {
        return;
    }

    ++syncGeneration;
    identity = nextIdentity;
    remoteSyncDisabled = false;
    pendingTeamSyncs = 0;
    teamSyncHadError = false;
    remoteKeysSeen.clear();
    entries.clear();

    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    storePath = QDir(base).filePath(
        QStringLiteral("drafts/") + identity + QStringLiteral(".json"));

    bool loaded = false;
    const QVector<DraftEntry> stored = DraftStore::load(storePath, &loaded);
    if (!loaded) {
        qWarning() << "Failed to load draft store" << storePath;
        return;
    }
    for (const DraftEntry& entry : stored) {
        entries.insert(entry.key(), entry);
    }
}

QVector<DraftEntry> DraftService::drafts()
{
    ensureIdentity();

    QVector<DraftEntry> result;
    result.reserve(entries.size());
    for (const DraftEntry& entry : entries) {
        if (!entry.deleted) {
            result.push_back(entry);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const DraftEntry& left, const DraftEntry& right) {
        if (left.updateAt != right.updateAt) {
            return left.updateAt > right.updateAt;
        }
        return left.key() < right.key();
    });
    return result;
}

bool DraftService::findDraft(const QString& channelId, const QString& rootId,
                             DraftEntry& draft)
{
    ensureIdentity();

    const auto it = entries.constFind(entryKey(channelId, rootId));
    if (it == entries.cend() || it->deleted) {
        return false;
    }
    draft = *it;
    return true;
}

bool DraftService::findDraftByKey(const QString& key, DraftEntry& draft)
{
    ensureIdentity();

    const auto it = entries.constFind(key);
    if (it == entries.cend() || it->deleted) {
        return false;
    }
    draft = *it;
    return true;
}

bool DraftService::persist()
{
    QVector<DraftEntry> stored;
    stored.reserve(entries.size());
    for (const DraftEntry& entry : entries) {
        stored.push_back(entry);
    }
    if (!DraftStore::save(storePath, stored)) {
        qWarning() << "Failed to save draft store" << storePath;
        return false;
    }
    return true;
}

void DraftService::updateDraft(const QString& channelId, const QString& rootId,
                               const QString& message,
                               const QString& replyToPostId)
{
    ensureIdentity();
    if (channelId.isEmpty()) {
        return;
    }

    const QString key = entryKey(channelId, rootId);
    const auto existing = entries.constFind(key);

    if (message.isEmpty()) {
        if (existing == entries.cend()) {
            return;
        }
        if (!existing->remotePresent && !existing->syncRequested) {
            entries.remove(key);
            persist();
            emit draftChanged(channelId, rootId);
            emit draftsChanged();
            return;
        }

        DraftEntry tombstone = existing.value();
        if (tombstone.deleted && tombstone.message.isEmpty()
            && tombstone.replyToPostId.isEmpty()
            && !tombstone.syncRequested) {
            return;
        }
        tombstone.message.clear();
        tombstone.replyToPostId.clear();
        tombstone.updateAt = nextUpdateTime(&existing.value());
        tombstone.dirty = true;
        tombstone.deleted = true;
        tombstone.syncRequested = false;
        entries.insert(key, tombstone);
        persist();
        emit draftChanged(channelId, rootId);
        emit draftsChanged();
        return;
    }

    if (existing != entries.cend() && !existing->deleted
        && existing->message == message
        && existing->replyToPostId == replyToPostId) {
        return;
    }

    DraftEntry entry;
    entry.channelId = channelId;
    entry.rootId = rootId;
    entry.message = message;
    entry.replyToPostId = replyToPostId;
    entry.updateAt = nextUpdateTime(
        existing == entries.cend() ? nullptr : &existing.value());
    entry.dirty = true;
    entry.deleted = false;
    entry.remotePresent =
        existing != entries.cend() && existing->remotePresent;
    entry.syncRequested = false;
    entries.insert(key, entry);
    persist();

    qCInfo(lcDraftTrace).nospace()
        << "LOCAL_SAVE channel=" << channelId
        << " root=" << rootId
        << " chars=" << message.size()
        << " remotePresent=" << entry.remotePresent;

    emit draftChanged(channelId, rootId);
    emit draftsChanged();
}

void DraftService::flushDraft(const QString& channelId, const QString& rootId)
{
    ensureIdentity();
    if (channelId.isEmpty()) {
        return;
    }

    const QString key = entryKey(channelId, rootId);
    auto current = entries.find(key);
    if (current == entries.end() || current->isRecovered()) {
        return;
    }

    if (current->deleted && !current->remotePresent
        && !current->syncRequested) {
        entries.erase(current);
        persist();
        emit draftChanged(channelId, rootId);
        emit draftsChanged();
        return;
    }

    if (!current->dirty || remoteSyncDisabled) {
        return;
    }

    current->syncRequested = true;
    const DraftEntry snapshot = current.value();
    persist();

    qCInfo(lcDraftTrace).nospace()
        << "REMOTE_FLUSH channel=" << channelId
        << " root=" << rootId
        << " chars=" << snapshot.message.size()
        << " deleted=" << snapshot.deleted
        << " remotePresent=" << snapshot.remotePresent;

    syncEntry(snapshot);
}

void DraftService::removeDraft(const QString& channelId, const QString& rootId)
{
    ensureIdentity();
    if (channelId.isEmpty()) {
        return;
    }

    const QString key = entryKey(channelId, rootId);
    const auto existing = entries.constFind(key);
    if (existing == entries.cend()) {
        return;
    }

    if (remoteSyncDisabled
        || (!existing->remotePresent && !existing->syncRequested)) {
        entries.remove(key);
        persist();
        qCInfo(lcDraftTrace).nospace()
            << "LOCAL_DISCARD channel=" << channelId
            << " root=" << rootId
            << " reason=no-remote-copy";
        emit draftChanged(channelId, rootId);
        emit draftsChanged();
        return;
    }

    DraftEntry tombstone = existing.value();
    tombstone.message.clear();
    tombstone.replyToPostId.clear();
    tombstone.updateAt = nextUpdateTime(&existing.value());
    tombstone.dirty = true;
    tombstone.deleted = true;
    tombstone.syncRequested = true;
    entries.insert(key, tombstone);
    persist();

    qCInfo(lcDraftTrace).nospace()
        << "REMOTE_DELETE channel=" << channelId
        << " root=" << rootId;
    emit draftChanged(channelId, rootId);
    emit draftsChanged();
    syncEntry(tombstone);
}

QString DraftService::addRecoveredDraft(
    const QString& channelId,
    const QString& rootId,
    const QString& message,
    const QString& replyToPostId,
    const QStringList& attachmentPaths,
    const QString& recoveryId)
{
    ensureIdentity();
    if (channelId.isEmpty() || recoveryId.isEmpty()
        || (message.isEmpty() && attachmentPaths.isEmpty())) {
        return {};
    }

    DraftEntry entry;
    entry.channelId = channelId;
    entry.rootId = rootId;
    entry.message = message;
    entry.replyToPostId = replyToPostId;
    entry.attachmentPaths = attachmentPaths;
    entry.recoveryId = recoveryId;

    const QString key = entry.key();
    const auto existing = entries.constFind(key);
    if (existing != entries.cend() && !existing->deleted
        && existing->message == message
        && existing->replyToPostId == replyToPostId
        && existing->attachmentPaths == attachmentPaths) {
        return key;
    }

    entry.updateAt = nextUpdateTime(
        existing == entries.cend() ? nullptr : &existing.value());
    // Recovered entries never participate in Mattermost's one-draft-per-
    // conversation API. They are durable local recovery records only.
    entry.dirty = false;
    entry.remotePresent = false;
    entry.syncRequested = false;

    const bool hadExisting = existing != entries.cend();
    DraftEntry previous;
    if (hadExisting) {
        previous = existing.value();
    }

    entries.insert(key, entry);
    if (!persist()) {
        // The caller uses an empty result as "do not consume the durable outbox
        // record". Restore the in-memory map as well so a failed disk write is
        // not mistaken for successful recovery during this process lifetime.
        if (hadExisting) {
            entries.insert(key, previous);
        } else {
            entries.remove(key);
        }
        return {};
    }

    emit draftChanged(channelId, rootId);
    emit draftsChanged();
    return key;
}

void DraftService::updateRecoveredDraft(const QString& key,
                                        const QString& message,
                                        const QString& replyToPostId,
                                        const QStringList& attachmentPaths)
{
    ensureIdentity();
    auto existing = entries.find(key);
    if (existing == entries.end() || existing->deleted
        || !existing->isRecovered()) {
        return;
    }

    if (message.isEmpty() && attachmentPaths.isEmpty()) {
        removeDraftByKey(key);
        return;
    }
    if (existing->message == message
        && existing->replyToPostId == replyToPostId
        && existing->attachmentPaths == attachmentPaths) {
        return;
    }

    existing->message = message;
    existing->replyToPostId = replyToPostId;
    existing->attachmentPaths = attachmentPaths;
    existing->updateAt = nextUpdateTime(&existing.value());
    existing->dirty = false;
    existing->remotePresent = false;
    existing->syncRequested = false;
    const QString channelId = existing->channelId;
    const QString rootId = existing->rootId;
    persist();
    emit draftChanged(channelId, rootId);
    emit draftsChanged();
}

void DraftService::removeDraftByKey(const QString& key)
{
    ensureIdentity();
    auto existing = entries.find(key);
    if (existing == entries.end() || existing->deleted) {
        return;
    }

    if (!existing->isRecovered()) {
        const QString channelId = existing->channelId;
        const QString rootId = existing->rootId;
        removeDraft(channelId, rootId);
        return;
    }

    const QString channelId = existing->channelId;
    const QString rootId = existing->rootId;
    entries.erase(existing);
    persist();
    emit draftChanged(channelId, rootId);
    emit draftsChanged();
}

void DraftService::syncEntry(const DraftEntry& draft)
{
    ensureIdentity();
    if (draft.isRecovered() || remoteSyncDisabled || draft.channelId.isEmpty()
        || !draft.syncRequested) {
        return;
    }

    QJsonObject props;
    if (!draft.deleted && !draft.replyToPostId.isEmpty()) {
        props.insert(QString::fromLatin1(PostProps::ReplyToPostId),
                     draft.replyToPostId);
    }

    QJsonObject payload {
        {QStringLiteral("channel_id"), draft.channelId},
        {QStringLiteral("root_id"), draft.rootId},
        {QStringLiteral("message"),
         draft.deleted ? QString() : draft.message},
        {QStringLiteral("props"), props},
    };

    NetworkRequest request(QStringLiteral("drafts"));
    const QString key = draft.key();
    connector.post(
        request, QByteArrayCreator(payload),
        HttpResponseCallback(
            [this, key, draft](const QJsonDocument& document,
                               const QNetworkReply& reply) {
        if (draftApiUnsupported(reply)) {
            qCInfo(lcDraftTrace).nospace()
                << "REMOTE_ACK key=" << key
                << " result=unsupported";
            disableRemoteSync();
            return;
        }
        if (!httpSucceeded(reply)) {
            qCInfo(lcDraftTrace).nospace()
                << "REMOTE_ACK key=" << key
                << " result=error"
                << " http=" << httpStatus(reply)
                << " network=" << reply.error();
            return;
        }

        qCInfo(lcDraftTrace).nospace()
            << "REMOTE_ACK key=" << key
            << " result=ok"
            << " deleted=" << draft.deleted;

        auto current = entries.find(key);
        if (current == entries.end()) {
            return;
        }
        if (current->updateAt != draft.updateAt
            || current->message != draft.message
            || current->replyToPostId != draft.replyToPostId
            || current->deleted != draft.deleted) {
            if (!draft.deleted && current->deleted
                && current->syncRequested) {
                current->remotePresent = true;
                persist();
                syncEntry(current.value());
            }
            return;
        }

        if (draft.deleted) {
            entries.erase(current);
            persist();
            return;
        }

        current->dirty = false;
        current->remotePresent = true;
        current->syncRequested = false;
        const qint64 serverUpdateAt =
            document.object().value(QStringLiteral("update_at"))
                .toVariant().toLongLong();
        if (serverUpdateAt > 0) {
            current->updateAt = serverUpdateAt;
        }
        const QString channelId = current->channelId;
        const QString rootId = current->rootId;
        persist();
        emit draftChanged(channelId, rootId);
        emit draftsChanged();
    }));
}

void DraftService::applyRemoteDraftEvent(const QJsonObject& data, bool deleted)
{
    ensureIdentity();
    if (remoteSyncDisabled) {
        return;
    }

    const QJsonObject object = remoteDraftObject(data);
    if (object.isEmpty()) {
        return;
    }

    const QString userId =
        object.value(QStringLiteral("user_id")).toString();
    const QString loginUserId = backend.getLoginUser().id;
    if (!userId.isEmpty() && !loginUserId.isEmpty()
        && userId != loginUserId) {
        return;
    }

    DraftEntry remote = remoteDraft(object);
    if (remote.channelId.isEmpty()) {
        return;
    }

    qCInfo(lcDraftTrace).nospace()
        << "REMOTE_EVENT channel=" << remote.channelId
        << " root=" << remote.rootId
        << " deleted=" << deleted
        << " chars=" << remote.message.size()
        << " updateAt=" << remote.updateAt;

    const qint64 deleteAt =
        object.value(QStringLiteral("delete_at")).toVariant().toLongLong();
    deleted = deleted || deleteAt > 0;
    const QString key = remote.key();
    auto local = entries.find(key);

    if (deleted) {
        if (local == entries.end() || local->isRecovered()) {
            return;
        }

        if (local->dirty && !local->deleted) {
            local->remotePresent = false;
            persist();
            return;
        }

        const QString channelId = local->channelId;
        const QString rootId = local->rootId;
        entries.erase(local);
        persist();
        emit draftChanged(channelId, rootId);
        emit draftsChanged();
        return;
    }

    if (remote.updateAt <= 0
        || (remote.message.isEmpty() && remote.replyToPostId.isEmpty())) {
        return;
    }

    remote.dirty = false;
    remote.deleted = false;
    remote.remotePresent = true;
    remote.syncRequested = false;

    if (local != entries.end()) {
        if (local->isRecovered()) {
            return;
        }

        if (local->dirty) {
            local->remotePresent = true;
            if (local->syncRequested && !local->deleted
                && local->message == remote.message
                && local->replyToPostId == remote.replyToPostId) {
                local->dirty = false;
                local->syncRequested = false;
                local->updateAt = std::max(local->updateAt, remote.updateAt);
                persist();
                emit draftChanged(local->channelId, local->rootId);
                emit draftsChanged();
            } else {
                persist();
            }
            return;
        }

        if (local->updateAt > remote.updateAt) {
            return;
        }
        if (local->updateAt == remote.updateAt
            && local->message == remote.message
            && local->replyToPostId == remote.replyToPostId
            && !local->deleted && local->remotePresent) {
            return;
        }
    }

    entries.insert(key, remote);
    persist();
    emit draftChanged(remote.channelId, remote.rootId);
    emit draftsChanged();
}

void DraftService::syncAllTeams()
{
    ensureIdentity();
    if (remoteSyncDisabled || backend.getLoginUser().id.isEmpty()) {
        return;
    }

    QStringList teamIds;
    for (const auto& pair : backend.getStorage().teams) {
        teamIds.push_back(pair.first);
    }
    if (teamIds.isEmpty()) {
        return;
    }

    const quint64 generation = ++syncGeneration;
    pendingTeamSyncs = static_cast<int>(teamIds.size());
    teamSyncHadError = false;
    remoteKeysSeen.clear();

    for (const QString& teamId : teamIds) {
        NetworkRequest request(
            QStringLiteral("users/me/teams/") + teamId
            + QStringLiteral("/drafts"));
        connector.get(
            request,
            HttpResponseCallback(
                [this, generation](const QJsonDocument& document,
                                   const QNetworkReply& reply) {
            if (generation != syncGeneration) {
                return;
            }

            if (draftApiUnsupported(reply)) {
                disableRemoteSync();
                return;
            }

            bool changed = false;
            if (!httpSucceeded(reply) || !document.isArray()) {
                teamSyncHadError = true;
            } else {
                for (const QJsonValue& value : document.array()) {
                    DraftEntry remote = remoteDraft(value.toObject());
                    if (remote.channelId.isEmpty() || remote.updateAt <= 0
                        || (remote.message.isEmpty()
                            && remote.replyToPostId.isEmpty())) {
                        continue;
                    }

                    const QString key = remote.key();
                    remoteKeysSeen.insert(key);

                    remote.remotePresent = true;
                    remote.syncRequested = false;

                    auto local = entries.find(key);
                    if (local != entries.end() && local->dirty) {
                        if (!local->remotePresent) {
                            local->remotePresent = true;
                            changed = true;
                        }
                        continue;
                    }

                    if (local == entries.end()
                        || remote.updateAt > local->updateAt) {
                        entries.insert(key, remote);
                        changed = true;
                    }
                }
            }

            if (changed) {
                persist();
                emit draftsChanged();
            }

            --pendingTeamSyncs;
            if (pendingTeamSyncs == 0) {
                finishTeamSync(generation);
            }
        }));
    }
}

void DraftService::finishTeamSync(quint64 generation)
{
    if (generation != syncGeneration || remoteSyncDisabled
        || teamSyncHadError) {
        return;
    }

    QVector<DraftEntry> toSync;
    QVector<QString> toRemove;
    bool stateChanged = false;

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->isRecovered()) {
            continue;
        }

        const bool existsRemotely = remoteKeysSeen.contains(it.key());
        if (existsRemotely) {
            if (!it->remotePresent) {
                it->remotePresent = true;
                stateChanged = true;
            }
            if (it->dirty && it->syncRequested) {
                toSync.push_back(it.value());
            }
            continue;
        }

        if (it->remotePresent) {
            it->remotePresent = false;
            stateChanged = true;
        }

        if (it->dirty) {
            if (it->deleted) {
                toRemove.push_back(it.key());
            } else if (it->syncRequested) {
                toSync.push_back(it.value());
            }
        } else {
            toRemove.push_back(it.key());
        }
    }

    bool removed = false;
    for (const QString& key : toRemove) {
        if (entries.remove(key)) {
            removed = true;
        }
    }
    if (removed || stateChanged) {
        persist();
        emit draftsChanged();
    }

    for (const DraftEntry& entry : toSync) {
        syncEntry(entry);
    }
}

void DraftService::disableRemoteSync()
{
    if (remoteSyncDisabled) {
        return;
    }

    remoteSyncDisabled = true;
    ++syncGeneration;
    pendingTeamSyncs = 0;
    remoteKeysSeen.clear();

    bool changed = false;
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->deleted) {
            it = entries.erase(it);
            changed = true;
        } else {
            if (it->syncRequested || it->remotePresent) {
                it->syncRequested = false;
                it->remotePresent = false;
                changed = true;
            }
            ++it;
        }
    }
    if (changed) {
        persist();
        emit draftsChanged();
    }
}

} // namespace Mattermost
