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

void DraftService::persist()
{
    QVector<DraftEntry> stored;
    stored.reserve(entries.size());
    for (const DraftEntry& entry : entries) {
        stored.push_back(entry);
    }
    if (!DraftStore::save(storePath, stored)) {
        qWarning() << "Failed to save draft store" << storePath;
    }
}

void DraftService::updateDraft(const QString& channelId, const QString& rootId,
                               const QString& message,
                               const QString& replyToPostId)
{
    ensureIdentity();
    if (channelId.isEmpty()) {
        return;
    }

    // Mattermost's synced-drafts API treats an empty message as deletion.
    // A bare reply target without text therefore cannot be represented
    // consistently across clients and is not a standalone draft.
    if (message.isEmpty()) {
        removeDraft(channelId, rootId);
        return;
    }

    const QString key = entryKey(channelId, rootId);
    const auto existing = entries.constFind(key);
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
    entries.insert(key, entry);
    persist();

    emit draftChanged(channelId, rootId);
    emit draftsChanged();
    syncEntry(entry);
}

void DraftService::removeDraft(const QString& channelId, const QString& rootId)
{
    ensureIdentity();
    if (channelId.isEmpty()) {
        return;
    }

    const QString key = entryKey(channelId, rootId);
    const auto existing = entries.constFind(key);
    if (existing == entries.cend() || existing->deleted) {
        return;
    }

    DraftEntry tombstone = existing.value();
    tombstone.message.clear();
    tombstone.replyToPostId.clear();
    tombstone.updateAt = nextUpdateTime(&existing.value());
    tombstone.dirty = true;
    tombstone.deleted = true;
    entries.insert(key, tombstone);
    persist();

    emit draftChanged(channelId, rootId);
    emit draftsChanged();

    if (remoteSyncDisabled) {
        entries.remove(key);
        persist();
        return;
    }
    syncEntry(tombstone);
}

void DraftService::syncEntry(const DraftEntry& draft)
{
    ensureIdentity();
    if (remoteSyncDisabled || draft.channelId.isEmpty()) {
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
            disableRemoteSync();
            return;
        }
        if (!httpSucceeded(reply)) {
            return;
        }

        auto current = entries.find(key);
        if (current == entries.end()
            || current->updateAt != draft.updateAt
            || current->message != draft.message
            || current->replyToPostId != draft.replyToPostId
            || current->deleted != draft.deleted) {
            return;
        }

        if (draft.deleted) {
            entries.erase(current);
            persist();
            return;
        }

        current->dirty = false;
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

                    auto local = entries.find(key);
                    if (local != entries.end()
                        && local->deleted && local->dirty) {
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
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (remoteKeysSeen.contains(it.key())) {
            if (it->dirty) {
                toSync.push_back(it.value());
            }
            continue;
        }

        if (it->dirty) {
            toSync.push_back(it.value());
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
    if (removed) {
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
            ++it;
        }
    }
    if (changed) {
        persist();
        emit draftsChanged();
    }
}

} // namespace Mattermost
