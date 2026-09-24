/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "BusyIndicator.h"

#include <algorithm>

#include <QPainter>
#include <QPalette>
#include <QSizePolicy>

namespace Mattermost::BusyIndicator {

void draw(QPainter& painter,
          const QRectF& ring,
          int phase,
          const QColor& color,
          qreal penWidth)
{
    if (ring.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    const int normalizedPhase =
        ((phase % AnimationSteps) + AnimationSteps) % AnimationSteps;
    painter.drawArc(ring, (-90 + normalizedPhase * 30) * 16, 105 * 16);
    painter.restore();
}

Animation& Animation::instance()
{
    static Animation animation;
    return animation;
}

Animation::Animation()
{
    timer_.setInterval(AnimationIntervalMs);
    QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
        phase_ = (phase_ + 1) % AnimationSteps;
        frameCache_.clear();

        // update() only schedules paint; it does not synchronously render every
        // consumer. All visible widgets therefore share this one animation tick.
        const QSet<QWidget*> consumers = consumers_;
        for (QWidget* consumer : consumers) {
            if (consumer && consumers_.contains(consumer)) {
                consumer->update();
            }
        }
    });
}

void Animation::acquire(QWidget& consumer)
{
    if (consumers_.contains(&consumer)) {
        return;
    }

    consumers_.insert(&consumer);
    destructionConnections_.insert(
        &consumer,
        QObject::connect(
            &consumer, &QObject::destroyed, &timer_,
            [this, consumerPtr = &consumer] {
                consumers_.remove(consumerPtr);
                destructionConnections_.remove(consumerPtr);
                updateRunningState();
            }));
    updateRunningState();
    consumer.update();
}

void Animation::release(QWidget& consumer)
{
    if (consumers_.remove(&consumer) == 0) {
        return;
    }
    const auto connection = destructionConnections_.take(&consumer);
    QObject::disconnect(connection);
    updateRunningState();
}

void Animation::updateRunningState()
{
    if (consumers_.isEmpty()) {
        timer_.stop();
        phase_ = 0;
        frameCache_.clear();
    } else if (!timer_.isActive()) {
        timer_.start();
    }
}

QPixmap Animation::frame(int extent,
                         const QColor& color,
                         qreal penWidth,
                         qreal devicePixelRatio)
{
    extent = std::max(1, extent);
    devicePixelRatio = std::max<qreal>(1.0, devicePixelRatio);
    const QString key = QStringLiteral("%1:%2:%3:%4")
        .arg(extent)
        .arg(color.rgba())
        .arg(qRound(penWidth * 100.0))
        .arg(qRound(devicePixelRatio * 100.0));

    const auto cached = frameCache_.constFind(key);
    if (cached != frameCache_.cend()) {
        return cached.value();
    }

    const int pixelExtent =
        std::max(1, qRound(extent * devicePixelRatio));
    QPixmap pixmap(pixelExtent, pixelExtent);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    draw(painter, QRectF(0, 0, extent, extent), phase_, color, penWidth);

    frameCache_.insert(key, pixmap);
    return pixmap;
}

} // namespace Mattermost::BusyIndicator

namespace Mattermost {

BusyIndicatorWidget::BusyIndicatorWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(12, 12);
    hide();
}

BusyIndicatorWidget::~BusyIndicatorWidget()
{
    if (animating_) {
        BusyIndicator::Animation::instance().release(*this);
    }
}

void BusyIndicatorWidget::setAnimating(bool animating)
{
    if (animating_ == animating) {
        return;
    }

    animating_ = animating;
    if (animating_) {
        BusyIndicator::Animation::instance().acquire(*this);
        show();
    } else {
        BusyIndicator::Animation::instance().release(*this);
        hide();
    }
    update();
}

void BusyIndicatorWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    if (!animating_) {
        return;
    }

    QColor color = palette().color(QPalette::WindowText);
    color.setAlpha(190);

    const int outerExtent = std::min(width(), height());
    const int extent = std::max(4, outerExtent - 2);
    const qreal penWidth = std::max<qreal>(1.2, extent / 7.0);
    const QPixmap frame = BusyIndicator::Animation::instance().frame(
        extent, color, penWidth, devicePixelRatioF());

    QPainter painter(this);
    painter.drawPixmap(
        QPointF((width() - extent) / 2.0, (height() - extent) / 2.0),
        frame);
}

} // namespace Mattermost
