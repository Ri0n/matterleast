/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "PresenceAvatarLabel.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>

#include "AvatarUtils.h"
#include "BusyIndicator.h"

namespace Mattermost {

namespace {

constexpr int DefaultAvatarSize = 48;
constexpr int DefaultBadgeSize = 12;
constexpr qreal BadgeHitMargin = 3.0;

QColor applicationWindowColor()
{
    return qApp ? qApp->palette().color(QPalette::Window) : QColor();
}

} // namespace

PresenceAvatarLabel::PresenceAvatarLabel(QWidget* parent)
    : ClickableLabel(parent)
{
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignCenter);

    connectionAnimationTimer.setInterval(BusyIndicator::AnimationIntervalMs);
    connect(&connectionAnimationTimer, &QTimer::timeout, this, [this] {
        connectionAnimationPhase =
            (connectionAnimationPhase + 1) % BusyIndicator::AnimationSteps;
        update();
    });
}

void PresenceAvatarLabel::setPixmap(const QPixmap& pixmap)
{
    sourcePixmap = pixmap;
    refreshPixmap();
}

void PresenceAvatarLabel::setStatus(const QString& status)
{
    if (presenceStatus == status) {
        return;
    }
    presenceStatus = status;
    refreshPixmap();
}

void PresenceAvatarLabel::setConnectionIndicatorState(ConnectionIndicatorState state)
{
    if (connectionState == state) {
        return;
    }

    connectionState = state;
    if (connectionState == ConnectionIndicatorState::None) {
        connectionAnimationTimer.stop();
        connectionAnimationPhase = 0;
        setToolTip(QString());
    } else {
        if (!connectionAnimationTimer.isActive()) {
            connectionAnimationTimer.start();
        }
        setToolTip(connectionState == ConnectionIndicatorState::WaitingForReconnect
                       ? tr("Reconnecting to Mattermost… Click the indicator to retry now.")
                       : tr("Connecting to Mattermost…"));
    }

    refreshPixmap();
    update();
}

bool PresenceAvatarLabel::isPresenceStatus(const QString& text)
{
    return text == QStringLiteral("online")
        || text == QStringLiteral("away")
        || text == QStringLiteral("dnd")
        || text == QStringLiteral("offline");
}

void PresenceAvatarLabel::changeEvent(QEvent* event)
{
    ClickableLabel::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshPixmap();
    }
}

void PresenceAvatarLabel::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton
        && connectionState != ConnectionIndicatorState::None
        && !sourcePixmap.isNull()
        && connectionBadgeRect().adjusted(-BadgeHitMargin, -BadgeHitMargin,
                                          BadgeHitMargin, BadgeHitMargin)
               .contains(event->pos())) {
        QLabel::mouseReleaseEvent(event);
        if (connectionState == ConnectionIndicatorState::WaitingForReconnect) {
            emit reconnectRequested();
        }
        return;
    }

    ClickableLabel::mouseReleaseEvent(event);
}

void PresenceAvatarLabel::paintEvent(QPaintEvent* event)
{
    // The badge contains a one-pixel background ring. Validate that cached
    // pixmap at the actual paint boundary so a desktop palette transition can
    // never leave that ring in the previous theme's background colour.
    const QColor currentBackground = applicationWindowColor();
    if (renderedBackground != currentBackground) {
        refreshPixmap();
    }
    ClickableLabel::paintEvent(event);

    if (connectionState == ConnectionIndicatorState::None || sourcePixmap.isNull()) {
        return;
    }

    QPainter painter(this);
    const QRectF badge = connectionBadgeRect();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(renderedBackground);
    painter.drawEllipse(badge.adjusted(-1.0, -1.0, 1.0, 1.0));

    QColor spinnerColor = (qApp ? qApp->palette() : palette()).color(QPalette::WindowText);
    spinnerColor.setAlpha(190);
    const qreal inset = std::max<qreal>(1.5, badge.width() / 6.0);
    const qreal penWidth = std::max<qreal>(1.2, badge.width() / 7.0);
    BusyIndicator::draw(painter, badge.adjusted(inset, inset, -inset, -inset),
                        connectionAnimationPhase, spinnerColor, penWidth);
}

void PresenceAvatarLabel::resizeEvent(QResizeEvent* event)
{
    ClickableLabel::resizeEvent(event);
    refreshPixmap();
}

QRectF PresenceAvatarLabel::connectionBadgeRect() const
{
    int avatarSize = std::min(width(), height());
    if (avatarSize <= 0) {
        avatarSize = DefaultAvatarSize;
    }
    const int badgeSize = std::max(4,
        qRound(DefaultBadgeSize * avatarSize / static_cast<qreal>(DefaultAvatarSize)));
    const qreal avatarLeft = (width() - avatarSize) / 2.0;
    const qreal avatarTop = (height() - avatarSize) / 2.0;
    return QRectF(avatarLeft + avatarSize - badgeSize - 1,
                  avatarTop + avatarSize - badgeSize - 1,
                  badgeSize,
                  badgeSize);
}

void PresenceAvatarLabel::refreshPixmap()
{
    if (sourcePixmap.isNull()) {
        renderedBackground = applicationWindowColor();
        QLabel::clear();
        return;
    }

    int avatarSize = std::min(width(), height());
    if (avatarSize <= 0) {
        avatarSize = DefaultAvatarSize;
    }
    const int badgeSize = std::max(4,
        qRound(DefaultBadgeSize * avatarSize / static_cast<qreal>(DefaultAvatarSize)));

    renderedBackground = applicationWindowColor();
    if (connectionState == ConnectionIndicatorState::None) {
        QLabel::setPixmap(AvatarUtils::withStatus(sourcePixmap,
                                                   avatarSize,
                                                   presenceStatus,
                                                   badgeSize,
                                                   renderedBackground));
    } else {
        QLabel::setPixmap(AvatarUtils::circular(sourcePixmap, avatarSize));
    }
}

PresenceStatusLabel::PresenceStatusLabel(QWidget* parent)
    : QLabel(parent)
{
}

void PresenceStatusLabel::setText(const QString& text)
{
    if (PresenceAvatarLabel::isPresenceStatus(text)) {
        if (QWidget* host = parentWidget()) {
            if (auto* avatar = host->findChild<PresenceAvatarLabel*>(
                    QStringLiteral("usericon_label"))) {
                avatar->setStatus(text);
                QLabel::clear();
                hide();
                return;
            }
        }
    }

    QLabel::setText(text);
    setVisible(!text.isEmpty());
}

} // namespace Mattermost
