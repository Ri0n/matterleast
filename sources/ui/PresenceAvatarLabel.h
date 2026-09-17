/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QColor>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>

#include "ClickableLabel.h"

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace Mattermost {

/**
 * QLabel-compatible avatar that renders presence and connection state in the
 * lower-right badge. During initial connection/reconnect the presence badge is
 * replaced by the same compact spinner used by other busy UI actions.
 */
class PresenceAvatarLabel final : public ClickableLabel
{
    Q_OBJECT

public:
    enum class ConnectionIndicatorState {
        None,
        Connecting,
        WaitingForReconnect,
    };
    Q_ENUM(ConnectionIndicatorState)

    explicit PresenceAvatarLabel(QWidget* parent = nullptr);

    void setPixmap(const QPixmap& pixmap);
    void setStatus(const QString& status);
    void setConnectionIndicatorState(ConnectionIndicatorState state);
    ConnectionIndicatorState connectionIndicatorState() const { return connectionState; }

    static bool isPresenceStatus(const QString& text);

signals:
    void reconnectRequested();

protected:
    void changeEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRectF connectionBadgeRect() const;
    void refreshPixmap();

    QPixmap sourcePixmap;
    QString presenceStatus;
    QColor renderedBackground;
    QTimer connectionAnimationTimer;
    ConnectionIndicatorState connectionState = ConnectionIndicatorState::None;
    int connectionAnimationPhase = 0;
};

/**
 * Compatibility label for the sidebar header. Existing MainWindow code writes
 * online/away/dnd/offline as text; consume that value as avatar state instead
 * of displaying a second textual presence indicator.
 */
class PresenceStatusLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit PresenceStatusLabel(QWidget* parent = nullptr);

    void setText(const QString& text);
};

} // namespace Mattermost
