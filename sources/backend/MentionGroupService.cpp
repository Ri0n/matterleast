/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "MentionGroupService.h"

#include <algorithm>
#include <memory>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QNetworkReply>
#include <QUrl>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"

namespace Mattermost {
namespace {

constexpr int GroupMembersPerPage = 100;

QString displayName(const QJsonObject& user)
{
    const QString firstName = user.value(QStringLiteral("first_name")).toString();
    const QString lastName = user.value(QStringLiteral("last_name")).toString();
    if (!firstName.isEmpty()) {
        return lastName.isEmpty() ? firstName : firstName + QLatin1Char(' ') + lastName;
    }
    return user.value(QStringLiteral("username")).toString();
}

bool parseMentionGroup(const QJsonObject& object,
                       MentionGroup& group,
                       bool requireReference = true)
{
    group.id = object.value(QStringLiteral("id")).toString();
    group.name = object.value(QStringLiteral("name")).toString();
    group.displayName = object.value(QStringLiteral("display_name")).toString();
    group.description = object.value(QStringLiteral("description")).toString();
    group.source = object.value(QStringLiteral("source")).toString();
    group.deleteAt = object.value(QStringLiteral("delete_at")).toVariant().toLongLong();
    group.memberCount = object.value(QStringLiteral("member_count")).toInt();
    group.allowReference =
        object.value(QStringLiteral("allow_reference")).toBool(true);

    return !group.id.isEmpty()
        && !group.name.isEmpty()
        && (!requireReference || group.allowReference);
}

MentionGroupMutationResult mutationResult(const QByteArray& data,
                                          const QNetworkReply& reply)
{
    MentionGroupMutationResult result;
    result.ok = reply.error() == QNetworkReply::NoError;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonObject object = doc.object();
    if (result.ok) {
        parseMentionGroup(object, result.group, false);
    } else {
        result.errorId = object.value(QStringLiteral("id")).toString();
        result.errorMessage = object.value(QStringLiteral("message")).toString();
        if (result.errorMessage.isEmpty()) {
            result.errorMessage = reply.errorString();
        }
    }
    return result;
}

QString encodedQueryValue(const QString& value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

} // namespace

MentionGroupService& MentionGroupService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<MentionGroupService>> instances;
    QPointer<MentionGroupService>& service = instances[&backend];
    if (!service) {
        service = new MentionGroupService(backend);
    }
    return *service;
}

MentionGroupService::MentionGroupService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
}

void MentionGroupService::clear()
{
    groupsByTeamAndId.clear();
    loadedTeams.clear();
    loadingTeams.clear();
    teamWaiters.clear();
}

void MentionGroupService::ensureTeamGroups(const QString& teamId, GroupsCallback callback)
{
    if (teamId.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    if (loadedTeams.contains(teamId)) {
        if (callback) {
            callback();
        }
        return;
    }

    if (callback) {
        teamWaiters[teamId].push_back(std::move(callback));
    }
    if (loadingTeams.contains(teamId)) {
        return;
    }
    loadingTeams.insert(teamId);

    NetworkRequest request(
        QStringLiteral("teams/") + teamId
        + QStringLiteral("/groups?paginate=false&filter_allow_reference=true&include_member_count=true"));
    httpConnector.get(request, HttpResponseCallback(
        [this, teamId](const QJsonDocument& doc) {
            QHash<QString, MentionGroup> groups;
            const QJsonArray array = doc.object().value(QStringLiteral("groups")).toArray();
            for (const QJsonValue& value : array) {
                MentionGroup group;
                if (!parseMentionGroup(value.toObject(), group)) {
                    continue;
                }
                groups.insert(group.id, std::move(group));
            }
            groupsByTeamAndId.insert(teamId, std::move(groups));
            finishTeamLoad(teamId);
        }));
}

void MentionGroupService::searchReferenceGroups(const QString& teamId,
                                                     const QString& query,
                                                     const QString& channelId,
                                                     int limit,
                                                     GroupSearchCallback callback)
{
    if (query.isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    QString url = QStringLiteral("groups?q=")
        + encodedQueryValue(query)
        + QStringLiteral("&filter_allow_reference=true&page=0&per_page=")
        + QString::number(std::max(1, limit))
        + QStringLiteral("&include_member_count=true");
    if (!channelId.isEmpty()) {
        url += QStringLiteral("&include_channel_member_count=")
            + encodedQueryValue(channelId);
    }

    NetworkRequest request(url);
    httpConnector.get(request, HttpResponseCallback(
        [this, teamId, callback = std::move(callback)](const QJsonDocument& doc) mutable {
            QVector<MentionGroup> result;
            const QJsonArray array = doc.array();
            result.reserve(array.size());

            QHash<QString, MentionGroup>* cache = nullptr;
            if (!teamId.isEmpty()) {
                cache = &groupsByTeamAndId[teamId];
            }

            for (const QJsonValue& value : array) {
                MentionGroup group;
                if (!parseMentionGroup(value.toObject(), group)) {
                    continue;
                }

                if (cache) {
                    cache->insert(group.id, group);
                }
                result.push_back(std::move(group));
            }

            if (callback) {
                callback(std::move(result));
            }
        }));
}

void MentionGroupService::searchGroups(const QString& query,
                                           int limit,
                                           GroupSearchCallback callback)
{
    QString url = QStringLiteral("groups?q=")
        + encodedQueryValue(query)
        + QStringLiteral("&filter_allow_reference=false&include_archived=true")
        + QStringLiteral("&page=0&per_page=")
        + QString::number(std::max(1, limit))
        + QStringLiteral("&include_member_count=true");

    NetworkRequest request(url);
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback)](const QJsonDocument& doc) mutable {
            QVector<MentionGroup> result;
            const QJsonArray array = doc.array();
            result.reserve(array.size());

            for (const QJsonValue& value : array) {
                MentionGroup group;
                if (parseMentionGroup(value.toObject(), group, false)) {
                    result.push_back(std::move(group));
                }
            }

            if (callback) {
                callback(std::move(result));
            }
        }));
}

void MentionGroupService::createCustomGroup(const QString& displayName,
                                            const QString& mention,
                                            const QStringList& userIds,
                                            MutationCallback callback)
{
    QJsonArray users;
    for (const QString& userId : userIds) {
        if (!userId.isEmpty()) {
            users.push_back(userId);
        }
    }

    QJsonObject payload {
        {QStringLiteral("name"), mention},
        {QStringLiteral("display_name"), displayName},
        {QStringLiteral("allow_reference"), true},
        {QStringLiteral("source"), QStringLiteral("custom")},
        {QStringLiteral("user_ids"), users},
    };

    NetworkRequest request(QStringLiteral("groups"));
    httpConnector.post(
        request, QByteArrayCreator(payload),
        HttpResponseCallback(
            [this, callback = std::move(callback)](
                QVariant, QByteArray data, const QNetworkReply& reply) mutable {
                MentionGroupMutationResult result = mutationResult(data, reply);
                if (result.ok) {
                    invalidateCachesAfterMutation();
                }
                if (callback) {
                    callback(std::move(result));
                }
            }));
}

void MentionGroupService::updateCustomGroup(const QString& groupId,
                                            const QString& displayName,
                                            const QString& mention,
                                            MutationCallback callback)
{
    QJsonObject payload {
        {QStringLiteral("name"), mention},
        {QStringLiteral("display_name"), displayName},
    };

    NetworkRequest request(
        QStringLiteral("groups/") + groupId + QStringLiteral("/patch"));
    httpConnector.put(
        request, QByteArrayCreator(payload),
        HttpResponseCallback(
            [this, callback = std::move(callback)](
                QVariant, QByteArray data, const QNetworkReply& reply) mutable {
                MentionGroupMutationResult result = mutationResult(data, reply);
                if (result.ok) {
                    invalidateCachesAfterMutation();
                }
                if (callback) {
                    callback(std::move(result));
                }
            }));
}

void MentionGroupService::addMembers(const QString& groupId,
                                     const QStringList& userIds,
                                     MutationCallback callback)
{
    QJsonArray users;
    for (const QString& userId : userIds) {
        if (!userId.isEmpty()) {
            users.push_back(userId);
        }
    }
    if (users.isEmpty()) {
        MentionGroupMutationResult result;
        result.ok = true;
        if (callback) {
            callback(std::move(result));
        }
        return;
    }

    NetworkRequest request(
        QStringLiteral("groups/") + groupId + QStringLiteral("/members"));
    httpConnector.post(
        request,
        QByteArrayCreator(QJsonObject {{QStringLiteral("user_ids"), users}}),
        HttpResponseCallback(
            [this, callback = std::move(callback)](
                QVariant, QByteArray data, const QNetworkReply& reply) mutable {
                MentionGroupMutationResult result = mutationResult(data, reply);
                // The members endpoint returns an array rather than a Group.
                if (reply.error() == QNetworkReply::NoError) {
                    result.ok = true;
                    invalidateCachesAfterMutation();
                }
                if (callback) {
                    callback(std::move(result));
                }
            }));
}

void MentionGroupService::removeMembers(const QString& groupId,
                                        const QStringList& userIds,
                                        MutationCallback callback)
{
    QJsonArray users;
    for (const QString& userId : userIds) {
        if (!userId.isEmpty()) {
            users.push_back(userId);
        }
    }
    if (users.isEmpty()) {
        MentionGroupMutationResult result;
        result.ok = true;
        if (callback) {
            callback(std::move(result));
        }
        return;
    }

    NetworkRequest request(
        QStringLiteral("groups/") + groupId + QStringLiteral("/members"));
    httpConnector.del(
        request,
        QByteArrayCreator(QJsonObject {{QStringLiteral("user_ids"), users}}),
        HttpResponseCallback(
            [this, callback = std::move(callback)](
                QVariant, QByteArray data, const QNetworkReply& reply) mutable {
                MentionGroupMutationResult result = mutationResult(data, reply);
                if (reply.error() == QNetworkReply::NoError) {
                    result.ok = true;
                    invalidateCachesAfterMutation();
                }
                if (callback) {
                    callback(std::move(result));
                }
            }));
}

void MentionGroupService::invalidateCachesAfterMutation()
{
    QSet<QString> affectedTeams;
    const auto cachedTeamIds = groupsByTeamAndId.keys();
    for (const QString& teamId : cachedTeamIds) {
        if (!teamId.isEmpty()) {
            affectedTeams.insert(teamId);
        }
    }
    const QString currentTeamId = backend.getCurrentTeamContextId();
    if (!currentTeamId.isEmpty()) {
        affectedTeams.insert(currentTeamId);
    }

    groupsByTeamAndId.clear();
    loadedTeams.clear();

    for (const QString& teamId : affectedTeams) {
        emit groupsChanged(teamId);
    }
}

void MentionGroupService::finishTeamLoad(const QString& teamId)
{
    loadingTeams.remove(teamId);
    loadedTeams.insert(teamId);

    QVector<GroupsCallback> callbacks = teamWaiters.take(teamId);
    emit groupsChanged(teamId);
    for (auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

QHash<QString, QString> MentionGroupService::mentionIds(const QString& teamId) const
{
    QHash<QString, QString> result;
    const auto teamIt = groupsByTeamAndId.constFind(teamId);
    if (teamIt == groupsByTeamAndId.cend()) {
        return result;
    }

    for (auto it = teamIt->cbegin(); it != teamIt->cend(); ++it) {
        result.insert(it->name.toLower(), it->id);
    }
    return result;
}

const MentionGroup* MentionGroupService::groupById(const QString& teamId,
                                                   const QString& groupId) const
{
    const auto teamIt = groupsByTeamAndId.constFind(teamId);
    if (teamIt == groupsByTeamAndId.cend()) {
        return nullptr;
    }
    const auto groupIt = teamIt->constFind(groupId);
    return groupIt == teamIt->cend() ? nullptr : &groupIt.value();
}

void MentionGroupService::retrieveMembers(const QString& groupId, MembersCallback callback)
{
    if (groupId.isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    auto members = std::make_shared<QVector<MentionGroupMember>>();
    auto fetchPage = std::make_shared<std::function<void(int)>>();
    *fetchPage = [this, groupId, callback = std::move(callback), members, fetchPage](int page) mutable {
        NetworkRequest request(
            QStringLiteral("users?in_group=") + groupId
            + QStringLiteral("&page=") + QString::number(page)
            + QStringLiteral("&per_page=") + QString::number(GroupMembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [callback, members, fetchPage, page](const QJsonDocument& doc) mutable {
                const QJsonArray array = doc.array();
                members->reserve(members->size() + array.size());
                for (const QJsonValue& value : array) {
                    const QJsonObject object = value.toObject();
                    MentionGroupMember member;
                    member.id = object.value(QStringLiteral("id")).toString();
                    member.username = object.value(QStringLiteral("username")).toString();
                    member.displayName = displayName(object);
                    if (!member.id.isEmpty()) {
                        members->push_back(std::move(member));
                    }
                }

                if (array.size() == GroupMembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                std::sort(members->begin(), members->end(),
                          [](const MentionGroupMember& lhs, const MentionGroupMember& rhs) {
                    return lhs.displayName.compare(rhs.displayName, Qt::CaseInsensitive) < 0;
                });
                if (callback) {
                    callback(std::move(*members));
                }
            }));
    };

    (*fetchPage)(0);
}

} // namespace Mattermost
