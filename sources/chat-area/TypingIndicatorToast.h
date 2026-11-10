#pragma once

#include <algorithm>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QString>
#include <QWidget>

namespace Mattermost {

// Viewport child, never a row or a layout item. Its round frame, accent dots
// and subtle shadow deliberately distinguish typing from message bubbles.
class TypingIndicatorToast final : public QWidget
{
public:
    explicit TypingIndicatorToast(QWidget* viewport)
        : QWidget(viewport)
    {
        setObjectName(QStringLiteral("typingIndicatorToast"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

    void setMessage(QString message)
    {
        if (_message == message) {
            return;
        }
        _message = std::move(message);
        update();
    }

    int preferredWidth() const
    {
        return std::max(100, std::min(360,
            fontMetrics().horizontalAdvance(_message) + 60));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF panel(2, 2, width() - 6, height() - 8);
        if (panel.width() <= 0 || panel.height() <= 0) {
            return;
        }

        QColor shadow = palette().color(QPalette::Shadow);
        shadow.setAlpha(38);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shadow);
        painter.drawRoundedRect(panel.translated(1, 3), 13, 13);

        QColor surface = palette().color(QPalette::Window);
        surface = surface.lightness() < 128
            ? surface.lighter(118) : surface.darker(103);
        surface.setAlpha(249);
        QColor border = palette().color(QPalette::Mid);
        border.setAlpha(160);
        painter.setPen(QPen(border, 1));
        painter.setBrush(surface);
        painter.drawRoundedRect(panel, 13, 13);

        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        const qreal centerY = panel.center().y();
        for (int i = 0; i < 3; ++i) {
            painter.drawEllipse(QPointF(16 + i * 6, centerY), 1.6, 1.6);
        }

        painter.setPen(palette().color(QPalette::WindowText));
        const QRect textRect(41, 2, std::max(0, width() - 52), height() - 9);
        const QString text = fontMetrics().elidedText(
            _message, Qt::ElideRight, textRect.width());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
    }

private:
    QString _message;
};

} // namespace Mattermost
