/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "DraftStore.h"
#include "LocalMessageCrypto.h"

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
    QString result = channelId + QChar(0x1f) + rootId;
    if (!recoveryId.isEmpty()) {
        result += QChar(0x1f) + recoveryId;
    }
    return result;
}

QVector<DraftEntry> DraftStore::load(const QString& path, const QByteArray& key,
                                    bool* ok)
{
    if (ok) {
        *ok = false;
    }

    if (key.size() != LocalMessageCrypto::KeyBytes
        || !LocalMessageCrypto::isAvailable()) {
        return {};
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
    const int version = root.value(QStringLiteral("version")).toInt();
    if ((version != 1 && version != 2)
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
        const QString storedMessage = object.value(QStringLiteral("message")).toString();
        if (version == 2 && !storedMessage.isEmpty()) {
            const QByteArray envelope = QByteArray::fromBase64(
                storedMessage.toLatin1());
            QByteArray decrypted;
            // Derive the context from the unencrypted identifying fields.
            const QString recoveryId = object.value(QStringLiteral("recovery_id")).toString();
            const QByteArray entryContext = QByteArrayLiteral("draft-v1:")
                + (entry.channelId + QChar(0x1f) + entry.rootId
                   + (recoveryId.isEmpty() ? QString()
                      : QString(QChar(0x1f)) + recoveryId)).toUtf8();
            if (!LocalMessageCrypto::decrypt(envelope, key, entryContext,
                                              &decrypted)) {
                return {}; // never turn an undecryptable draft into an empty draft
            }
            entry.message = QString::fromUtf8(decrypted);
        } else {
            entry.message = storedMessage; // plaintext v1 is read only for migration
        }
        entry.replyToPostId =
            object.value(QStringLiteral("reply_to_post_id")).toString();
        const QJsonArray attachmentValues =
            object.value(QStringLiteral("attachment_paths")).toArray();
        entry.attachmentPaths.reserve(attachmentValues.size());
        for (const QJsonValue& attachmentValue : attachmentValues) {
            const QString attachmentPath = attachmentValue.toString();
            if (!attachmentPath.isEmpty()) {
                entry.attachmentPaths.push_back(attachmentPath);
            }
        }
        entry.recoveryId =
            object.value(QStringLiteral("recovery_id")).toString();
        entry.updateAt =
            object.value(QStringLiteral("update_at")).toVariant().toLongLong();
        entry.dirty = object.value(QStringLiteral("dirty")).toBool(false);
        entry.deleted = object.value(QStringLiteral("deleted")).toBool(false);
        if (object.contains(QStringLiteral("remote_present"))) {
            entry.remotePresent =
                object.value(QStringLiteral("remote_present")).toBool(false);
        } else {
            entry.remotePresent = entry.recoveryId.isEmpty()
                && (!entry.dirty || entry.deleted);
        }
        if (object.contains(QStringLiteral("sync_requested"))) {
            entry.syncRequested =
                object.value(QStringLiteral("sync_requested")).toBool(false);
        } else {
            entry.syncRequested =
                entry.recoveryId.isEmpty() && entry.dirty;
        }

        if (entry.channelId.isEmpty() || entry.updateAt <= 0) {
            continue;
        }
        if (!entry.deleted && entry.message.isEmpty()
            && entry.replyToPostId.isEmpty()
            && entry.attachmentPaths.isEmpty()) {
            continue;
        }
        result.push_back(std::move(entry));
    }

    if (ok) {
        *ok = true;
    }
    return result;
}

bool DraftStore::save(const QString& path, const QVector<DraftEntry>& drafts,
                      const QByteArray& key)
{
    if (key.size() != LocalMessageCrypto::KeyBytes
        || !LocalMessageCrypto::isAvailable()) {
        return false; // never write plaintext content without a key
    }
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QJsonArray array;
    for (const DraftEntry& entry : drafts) {
        QJsonArray attachmentPaths;
        for (const QString& attachmentPath : entry.attachmentPaths) {
            attachmentPaths.push_back(attachmentPath);
        }
        QString storedMessage;
        if (!entry.message.isEmpty()) {
            const QByteArray ciphertext = LocalMessageCrypto::encrypt(
                entry.message.toUtf8(), key,
                QByteArrayLiteral("draft-v1:") + entry.key().toUtf8());
            if (ciphertext.isEmpty()) return false;
            storedMessage = QString::fromLatin1(ciphertext.toBase64());
        }
        QJsonObject object {
            {QStringLiteral("channel_id"), entry.channelId},
            {QStringLiteral("root_id"), entry.rootId},
            {QStringLiteral("message"), storedMessage},
            {QStringLiteral("reply_to_post_id"), entry.replyToPostId},
            {QStringLiteral("attachment_paths"), attachmentPaths},
            {QStringLiteral("recovery_id"), entry.recoveryId},
            {QStringLiteral("update_at"),
             QJsonValue::fromVariant(QVariant::fromValue(entry.updateAt))},
            {QStringLiteral("dirty"), entry.dirty},
            {QStringLiteral("deleted"), entry.deleted},
            {QStringLiteral("remote_present"), entry.remotePresent},
            {QStringLiteral("sync_requested"), entry.syncRequested},
        };
        array.push_back(object);
    }

    const QJsonDocument document(QJsonObject {
        {QStringLiteral("version"), 2},
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
