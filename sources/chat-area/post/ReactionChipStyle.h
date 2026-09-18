#pragma once

#include <algorithm>

#include <QFont>
#include <QHBoxLayout>
#include <QString>
#include <QWidget>

namespace Mattermost::ReactionChipStyle {

constexpr int Height = 24;
constexpr int IconExtent = 20;
constexpr int IconPointSize = 14;
constexpr int CountPointSize = 8;
constexpr qreal ReferencePointSize = 10.0;
constexpr qreal CountScale = 0.75;
constexpr qreal MinCountPointSize = 8.0;
constexpr qreal MaxCountPointSize = 24.0;
constexpr int MinIconExtent = 16;
constexpr int MaxIconExtent = 32;

inline int iconExtent(const QFont& chatFont)
{
    qreal pointSize = chatFont.pointSizeF();
    if (pointSize <= 0.0 && chatFont.pixelSize() > 0) {
        pointSize = chatFont.pixelSize() * 72.0 / 96.0;
    }
    if (pointSize <= 0.0) {
        pointSize = ReferencePointSize;
    }

    return std::clamp(
        qRound(IconExtent * pointSize / ReferencePointSize),
        MinIconExtent,
        MaxIconExtent);
}

inline int chipHeight(const QFont& chatFont)
{
    return std::max(Height, iconExtent(chatFont) + 4);
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
    layout->setContentsMargins(4, 2, 4, 1);
    layout->setSpacing(2);
    updateMetrics(widget, layout, widget->font());
}

} // namespace Mattermost::ReactionChipStyle
