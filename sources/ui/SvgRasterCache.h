#pragma once

#include <algorithm>
#include <limits>

#include <QDateTime>
#include <QHash>
#include <QImageReader>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTimer>

namespace Mattermost {

/**
 * GUI-thread cache for SVG resources rasterized at their final device size.
 *
 * Cache entries are deliberately untinted. Callers may apply palette/state
 * colors after lookup without multiplying the cache by hover/checked/theme
 * variants. The key includes the logical requested size and DPR so no cached
 * bitmap is ever rescaled for a different screen scale.
 */
class SvgRasterCache final
{
public:
    static SvgRasterCache& instance()
    {
        static SvgRasterCache cache;
        return cache;
    }

    QPixmap raster(const QString& resource,
                   const QSize& logicalSize,
                   qreal devicePixelRatio)
    {
        if (resource.isEmpty() || !logicalSize.isValid()
            || logicalSize.isEmpty()) {
            return {};
        }

        const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
        const QSize physicalSize(
            std::max(1, qRound(logicalSize.width() * dpr)),
            std::max(1, qRound(logicalSize.height() * dpr)));
        const int dprMilli = qRound(dpr * 1000.0);
        const QString key = QStringLiteral("%1|%2x%3|%4x%5|%6")
                                .arg(resource)
                                .arg(logicalSize.width())
                                .arg(logicalSize.height())
                                .arg(physicalSize.width())
                                .arg(physicalSize.height())
                                .arg(dprMilli);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->lastUsedMs = now;
            it->accessSerial = ++accessSerial_;
            return it->pixmap;
        }

        QImageReader reader(resource);
        // qrc aliases intentionally omit the .svg suffix. Inspect the payload
        // so the SVG image-format plugin is selected by content.
        reader.setDecideFormatFromContent(true);
        reader.setScaledSize(physicalSize);
        QImage image = reader.read();
        if (image.isNull()) {
            return {};
        }

        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(dpr);

        const qint64 cost = static_cast<qint64>(pixmap.width())
            * static_cast<qint64>(pixmap.height()) * 4;
        if (cost > MaxCacheBytes) {
            return pixmap;
        }

        Entry entry;
        entry.pixmap = pixmap;
        entry.lastUsedMs = now;
        entry.accessSerial = ++accessSerial_;
        entry.costBytes = cost;
        entries_.insert(key, entry);
        cacheBytes_ += cost;
        evictToLimit();
        return pixmap;
    }

    void pruneStale(qint64 nowMs = QDateTime::currentMSecsSinceEpoch())
    {
        const qint64 cutoff = nowMs - MaxIdleMs;
        auto it = entries_.begin();
        while (it != entries_.end()) {
            if (it->lastUsedMs < cutoff) {
                cacheBytes_ -= it->costBytes;
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        QPixmap pixmap;
        qint64 lastUsedMs = 0;
        quint64 accessSerial = 0;
        qint64 costBytes = 0;
    };

    static constexpr qint64 MaxCacheBytes = 8 * 1024 * 1024;
    static constexpr qint64 MaxIdleMs = 60LL * 60 * 1000;
    static constexpr int CleanupIntervalMs = 60 * 60 * 1000;

    SvgRasterCache()
    {
        cleanupTimer_.setInterval(CleanupIntervalMs);
        cleanupTimer_.setTimerType(Qt::VeryCoarseTimer);
        QObject::connect(&cleanupTimer_, &QTimer::timeout, [] {
            SvgRasterCache::instance().pruneStale();
        });
        cleanupTimer_.start();
    }

    void evictToLimit()
    {
        while (cacheBytes_ > MaxCacheBytes && !entries_.isEmpty()) {
            auto oldest = entries_.end();
            quint64 oldestSerial = std::numeric_limits<quint64>::max();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->accessSerial < oldestSerial) {
                    oldestSerial = it->accessSerial;
                    oldest = it;
                }
            }
            if (oldest == entries_.end()) {
                break;
            }
            cacheBytes_ -= oldest->costBytes;
            entries_.erase(oldest);
        }
    }

    QHash<QString, Entry> entries_;
    QTimer cleanupTimer_;
    qint64 cacheBytes_ = 0;
    quint64 accessSerial_ = 0;
};

} // namespace Mattermost
