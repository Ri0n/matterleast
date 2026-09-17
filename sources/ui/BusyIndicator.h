/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QColor>
#include <QPainter>

namespace Mattermost::BusyIndicator {

inline constexpr int AnimationIntervalMs = 70;
inline constexpr int AnimationSteps = 12;

inline void draw(QPainter& painter,
                 const QRectF& ring,
                 int phase,
                 const QColor& color,
                 qreal penWidth = 2.0)
{
    if (ring.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    const int normalizedPhase = ((phase % AnimationSteps) + AnimationSteps) % AnimationSteps;
    painter.drawArc(ring, (-90 + normalizedPhase * 30) * 16, 105 * 16);
    painter.restore();
}

} // namespace Mattermost::BusyIndicator
