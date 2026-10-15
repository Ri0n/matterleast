#pragma once

#include <QtGlobal>

namespace MatterLeast::SeasonalSnow {

constexpr int DefaultSizePercent = 150;
constexpr int DefaultIntensityPercent = 100;

inline int flakeCountForArea(int width, int height, int intensityPercent)
{
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const int base = qBound(6, width * height / 52000, 22);
    const int intensity = qBound(25, intensityPercent, 250);
    return qBound(1, qRound(base * intensity / 100.0), 55);
}

inline double radiusScale(int sizePercent)
{
    return qBound(50, sizePercent, 300) / 100.0;
}

} // namespace MatterLeast::SeasonalSnow
