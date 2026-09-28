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

#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

struct MentionGroup {
    QString id;
    QString name;
    QString displayName;
    QString description;
    QString source;
    qint64 deleteAt = 0;
    int memberCount = 0;
    bool allowReference = true;

    bool isCustom() const { return source == QStringLiteral("custom"); }
    bool isArchived() const { return deleteAt != 0; }
};

struct MentionGroupMutationResult {
    bool ok = false;
    MentionGroup group;
    QString errorId;
    QString errorMessage;
};

struct MentionGroupMember {
    QString id;
    QString username;
    QString displayName;
};

class MentionGroupService final : public QObject
{
    Q_OBJECT
public:
    using GroupsCallback = std::function<void()>;
    using GroupSearchCallback = std::function<void(QVector<MentionGroup>)>;
    using MembersCallback = std::function<void(QVector<MentionGroupMember>)>;
    using MutationCallback = std::function<void(MentionGroupMutationResult)>;

    static MentionGroupService& instance(Backend& backend);

    void ensureTeamGroups(const QString& teamId, GroupsCallback callback = {});
    /**
     * Search the same global referenceable-group endpoint used by the webapp
     * mention provider. Results are also folded into the team-scoped mention
     * cache so a group selected from autocomplete can be linkified afterwards.
     */
    void searchReferenceGroups(const QString& teamId,
                               const QString& query,
                               const QString& channelId,
                               int limit,
                               GroupSearchCallback callback);
    void searchGroups(const QString& query,
                      int limit,
                      GroupSearchCallback callback);
    void createCustomGroup(const QString& displayName,
                           const QString& mention,
                           const QStringList& userIds,
                           MutationCallback callback);
    void updateCustomGroup(const QString& groupId,
                           const QString& displayName,
                           const QString& mention,
                           MutationCallback callback);
    void addMembers(const QString& groupId,
                    const QStringList& userIds,
                    MutationCallback callback);
    void removeMembers(const QString& groupId,
                       const QStringList& userIds,
                       MutationCallback callback);
    QHash<QString, QString> mentionIds(const QString& teamId) const;
    const MentionGroup* groupById(const QString& teamId, const QString& groupId) const;
    void retrieveMembers(const QString& groupId, MembersCallback callback);
    void clear();

signals:
    void groupsChanged(const QString& teamId);

private:
    explicit MentionGroupService(Backend& backend);
    void finishTeamLoad(const QString& teamId);
    void invalidateCachesAfterMutation();

    Backend& backend;
    HTTPConnector httpConnector;
    QHash<QString, QHash<QString, MentionGroup>> groupsByTeamAndId;
    QSet<QString> loadedTeams;
    QSet<QString> loadingTeams;
    QHash<QString, QVector<GroupsCallback>> teamWaiters;
    quint64 cacheGeneration = 0;
};

} // namespace Mattermost
