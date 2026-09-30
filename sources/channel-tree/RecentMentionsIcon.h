#pragma once

#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace Mattermost {

inline QPixmap recentMentionsPixmap(const QColor& color)
{
    constexpr int Extent = 24;
    QPixmap pixmap(Extent, Extent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(18);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("@"));
    return pixmap;
}

inline QIcon recentMentionsIcon(const QPalette& palette)
{
    QIcon icon;
    icon.addPixmap(recentMentionsPixmap(palette.color(QPalette::Text)),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(recentMentionsPixmap(palette.color(QPalette::HighlightedText)),
                   QIcon::Selected, QIcon::Off);
    icon.addPixmap(recentMentionsPixmap(
                       palette.color(QPalette::Disabled, QPalette::Text)),
                   QIcon::Disabled, QIcon::Off);
    return icon;
}

} // namespace Mattermost
