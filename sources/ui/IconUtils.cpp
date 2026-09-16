/**
 * Copyright 2026 Sergei Ilinykh
 *
 * Adapted from AnyKeep's IconUtils by the same author.
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#include "IconUtils.h"

#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

namespace Mattermost {

bool IconUtils::isDarkColorScheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) {
        return true;
    }
    if (scheme == Qt::ColorScheme::Light) {
        return false;
    }
#endif

    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QIcon IconUtils::tintedIcon(const QIcon& source, const QColor& color)
{
    if (source.isNull() || !color.isValid()) {
        return {};
    }

    QIcon icon;
    for (int size : {16, 20, 22, 24, 32, 48}) {
        QPixmap pixmap = source.pixmap(size, size);
        if (pixmap.isNull()) {
            continue;
        }

        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon IconUtils::tintedSymbolicIcon(const QString& path, const QColor& color)
{
    return tintedIcon(QIcon(path), color);
}

QIcon IconUtils::symbolicIcon(const QString& path)
{
    const QColor color = QGuiApplication::palette().color(QPalette::WindowText);
    QIcon icon = tintedSymbolicIcon(path, color);
    return icon.isNull() ? QIcon(path) : icon;
}

QIcon IconUtils::applicationIcon(uint32_t notificationCount)
{
    const QIcon source(QStringLiteral(":/icons/matterleast"));
    if (notificationCount == 0 || source.isNull()) {
        return source;
    }

    const QString badgeText = notificationCount > 99
        ? QStringLiteral("99+")
        : QString::number(notificationCount);

    QIcon icon;
    for (int size : {16, 20, 22, 24, 32, 48, 64, 128, 256}) {
        QPixmap pixmap = source.pixmap(size, size);
        if (pixmap.isNull()) {
            continue;
        }

        const qreal scale = size / 256.0;
        const qreal badgeHeight = std::max<qreal>(9.0, 80.0 * scale);
        const qreal horizontalPadding = std::max<qreal>(2.0, 12.0 * scale);

        QFont font = QApplication::font();
        font.setBold(true);
        font.setPixelSize(std::max(6, qRound(48.0 * scale)));
        const QFontMetricsF metrics(font);
        const qreal badgeWidth = std::max(
            badgeHeight, metrics.horizontalAdvance(badgeText) + 2.0 * horizontalPadding);
        const QRectF badgeRect(size - badgeWidth, 0.0, badgeWidth, badgeHeight);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(220, 53, 69));
        painter.drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);

        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(badgeRect, Qt::AlignCenter, badgeText);
        icon.addPixmap(pixmap);
    }

    return icon.isNull() ? source : icon;
}

} // namespace Mattermost
