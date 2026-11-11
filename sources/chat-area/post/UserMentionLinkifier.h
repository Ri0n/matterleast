/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QString>

#include <functional>

class QObject;
class QTextDocument;

namespace Mattermost {
class Backend;
namespace UserMentionLinkifier {

void linkify(QTextDocument& document,
             const QHash<QString, QString>& groupMentionIds = {},
             const QString& teamName = QString());

void resolveChannelDisplayNames(QTextDocument& document, Backend& backend,
                                const QString& teamName, QObject* owner,
                                std::function<void()> changed = {});

QString linkifyHtmlTextSegment(
    const QString& text,
    const QHash<QString, QString>& groupMentionIds = {});

QString linkifyHtml(
    const QString& html,
    const QHash<QString, QString>& groupMentionIds = {});

} // namespace UserMentionLinkifier
} // namespace Mattermost
