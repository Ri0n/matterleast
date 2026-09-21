/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "DraftStore.h"

#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QVariant>

namespace Mattermost {

QString DraftEntry::key() const
{
    return channelId + QChar(0x1f) + rootId;
}

QVector<DraftEntry> DraftStore::load(const QString& path, bool* ok)
{
    if (ok) {
        *ok = false;
    }

    QFile file(path);
    if (!file.exists()) {
        if (ok) {
            *ok = true;
        }
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("drafts")).isArray()) {
        return {};
    }

    QVector<DraftEntry> result;
    const QJsonArray array = root.value(QStringLiteral("drafts")).toArray();
    result.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();

        DraftEntry entry;
        entry.channelId = object.value(QStringLiteral("channel_id")).toString();
        entry.rootId = object.value(QStringLiteral("root_id")).toString();
        entry.message = object.value(QStringLiteral("message")).toString();
        entry.replyToPostId =
            object.value(QStringLiteral("reply_to_post_id")).toString();
        entry.updateAt =
            object.value(QStringLiteral("update_at")).toVariant().toLongLong();
        entry.dirty = object.value(QStringLiteral("dirty")).toBool(false);
        entry.deleted = object.value(QStringLiteral("deleted")).toBool(false);

        if (entry.channelId.isEmpty() || entry.updateAt <= 0) {
            continue;
        }
        if (!entry.deleted && entry.message.isEmpty()
            && entry.replyToPostId.isEmpty()) {
            continue;
        }
        result.push_back(std::move(entry));
    }

    if (ok) {
        *ok = true;
    }
    return result;
}

bool DraftStore::save(const QString& path, const QVector<DraftEntry>& drafts)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QJsonArray array;
    for (const DraftEntry& entry : drafts) {
        QJsonObject object {
            {QStringLiteral("channel_id"), entry.channelId},
            {QStringLiteral("root_id"), entry.rootId},
            {QStringLiteral("message"), entry.message},
            {QStringLiteral("reply_to_post_id"), entry.replyToPostId},
            {QStringLiteral("update_at"),
             QJsonValue::fromVariant(QVariant::fromValue(entry.updateAt))},
            {QStringLiteral("dirty"), entry.dirty},
            {QStringLiteral("deleted"), entry.deleted},
        };
        array.push_back(object);
    }

    const QJsonDocument document(QJsonObject {
        {QStringLiteral("version"), 1},
        {QStringLiteral("drafts"), array},
    });

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(document.toJson(QJsonDocument::Compact)) < 0) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace Mattermost
