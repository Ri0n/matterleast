#pragma once

#include <algorithm>

#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QString>
#include <QWidget>

namespace Mattermost::ReactionChipStyle {

constexpr int Height = 26;
constexpr int BorderWidth = 1;
constexpr int TopMargin = 2;
constexpr int BottomMargin = 1;
constexpr int IconPadding = 2;
constexpr qreal CountScale = 0.75;
constexpr qreal MinCountPointSize = 8.0;
constexpr qreal MaxCountPointSize = 24.0;

inline int iconExtent(const QFont& chatFont)
{
    // Reactions should read like normal inline text, not like enlarged emoji.
    // Use the actual chat-font line height as the visual emoji/image extent.
    return std::max(1, qRound(QFontMetricsF(chatFont).height()));
}

inline int iconBoxExtent(const QFont& chatFont)
{
    // Rich-text QLabel layout (used by custom/GIF emoji) needs a little room
    // around the image for line metrics and vertical-align. Keep that padding
    // outside the visual emoji extent so the emoji itself is not enlarged.
    return iconExtent(chatFont) + 2 * IconPadding;
}

inline int chipHeight(const QFont& chatFont)
{
    return std::max(
        Height,
        iconBoxExtent(chatFont)
            + TopMargin + BottomMargin + 2 * BorderWidth);
}

inline QFont countFont(QFont chatFont)
{
    if (chatFont.pointSizeF() > 0.0) {
        chatFont.setPointSizeF(std::clamp(
            chatFont.pointSizeF() * CountScale,
            MinCountPointSize,
            MaxCountPointSize));
    } else if (chatFont.pixelSize() > 0) {
        chatFont.setPixelSize(std::clamp(
            qRound(chatFont.pixelSize() * CountScale), 11, 32));
    }
    return chatFont;
}

inline void updateMetrics(QWidget* widget,
                          QHBoxLayout* layout,
                          const QFont& chatFont)
{
    if (!widget || !layout) {
        return;
    }

    const int height = chipHeight(chatFont);
    widget->setMinimumHeight(height);
    widget->setMaximumHeight(height);
}

inline QString styleSheet(const QString& objectName)
{
    return QStringLiteral(
        "QWidget#%1 {"
        " border: 1px solid rgba(128, 128, 128, 130);"
        " border-radius: 4px;"
        " background-color: rgba(128, 128, 128, 52);"
        " }"
        "QWidget#%1:hover {"
        " background-color: rgba(128, 128, 128, 72);"
        " }").arg(objectName);
}

inline void apply(QWidget* widget, QHBoxLayout* layout, const QString& objectName)
{
    if (!widget || !layout) {
        return;
    }
    widget->setObjectName(objectName);
    widget->setAttribute(Qt::WA_StyledBackground, true);
    widget->setCursor(Qt::PointingHandCursor);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    widget->setStyleSheet(styleSheet(objectName));
    layout->setContentsMargins(4, TopMargin, 4, BottomMargin);
    layout->setSpacing(2);
    updateMetrics(widget, layout, widget->font());
}

} // namespace Mattermost::ReactionChipStyle
