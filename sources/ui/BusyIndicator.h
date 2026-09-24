/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QPainter;
class QPaintEvent;

namespace Mattermost {

namespace BusyIndicator {

inline constexpr int AnimationIntervalMs = 70;
inline constexpr int AnimationSteps = 12;

void draw(QPainter& painter,
          const QRectF& ring,
          int phase,
          const QColor& color,
          qreal penWidth = 2.0);

/**
 * One animation clock and one rasterized frame cache for every busy indicator.
 *
 * Consumers explicitly acquire/release the animation. The timer runs only
 * while at least one widget wants animation. For a given phase/size/palette,
 * the first consumer rasterizes the spinner and all others reuse that pixmap.
 */
class Animation final : public QObject
{
public:
    static Animation& instance();

    void acquire(QWidget& consumer);
    void release(QWidget& consumer);

    QPixmap frame(int extent,
                  const QColor& color,
                  qreal penWidth = 2.0,
                  qreal devicePixelRatio = 1.0);

    bool isRunning() const { return timer_.isActive(); }
    int phase() const { return phase_; }

private:
    Animation();
    void updateRunningState();

    QTimer timer_;
    QSet<QWidget*> consumers_;
    QHash<QWidget*, QMetaObject::Connection> destructionConnections_;
    QHash<QString, QPixmap> frameCache_;
    int phase_ = 0;
};

} // namespace BusyIndicator

/** Compact shared busy spinner suitable for embedding into an existing row. */
class BusyIndicatorWidget final : public QWidget
{
public:
    explicit BusyIndicatorWidget(QWidget* parent = nullptr);
    ~BusyIndicatorWidget() override;

    void setAnimating(bool animating);
    bool isAnimating() const { return animating_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool animating_ = false;
};

} // namespace Mattermost
