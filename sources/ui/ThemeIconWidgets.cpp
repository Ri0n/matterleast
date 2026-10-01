/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "ThemeIconWidgets.h"

#include <algorithm>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QWidget>

#include "BusyIndicator.h"
#include "IconUtils.h"

namespace Mattermost {
namespace {

constexpr qreal RestingOpacity = 0.8;
constexpr int BusyIndicatorExtent = 18;
constexpr qreal FormattingToolbarOpticalScale = 1.20;
constexpr int FormattingToolbarButtonPadding = 8;
constexpr int FormattingToolbarHorizontalPadding = 10;

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

bool isFormattingToolbarIcon(const QString& objectName)
{
    return objectName == QStringLiteral("formatLinkButton")
        || objectName == QStringLiteral("formatBulletListButton")
        || objectName == QStringLiteral("formatNumberedListButton")
        || objectName == QStringLiteral("messagePriorityButton");
}

bool isFormattingToolbar(const QWidget* widget)
{
    return widget && widget->objectName() == QStringLiteral("formattingToolbar");
}

int formattingToolbarOpticalExtent(const QWidget& widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    const int capHeight = widget.fontMetrics().capHeight();
#else
    const int capHeight = widget.fontMetrics().height() * 3 / 4;
#endif
    return std::max(1, qRound(capHeight * FormattingToolbarOpticalScale));
}

void syncFormattingToolbarButtonGeometry(QWidget* toolbar)
{
    if (!isFormattingToolbar(toolbar)) {
        return;
    }

    const QFontMetrics metrics(toolbar->font());
    const int buttonHeight = std::max(
        28, metrics.height() + FormattingToolbarButtonPadding);

    const auto buttons = toolbar->findChildren<QAbstractButton*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QAbstractButton* button : buttons) {
        if (!button) {
            continue;
        }

        int buttonWidth = buttonHeight;
        if (!button->text().isEmpty()) {
            buttonWidth = std::max(
                buttonHeight,
                metrics.horizontalAdvance(button->text())
                    + FormattingToolbarHorizontalPadding);
        }

        const QSize target(buttonWidth, buttonHeight);
        if (button->minimumSize() != target
            || button->maximumSize() != target) {
            button->setMinimumSize(target);
            button->setMaximumSize(target);
            button->updateGeometry();
        }
    }
}

void drawLinkGlyph(QPainter& painter, const QRectF& bounds)
{
    const qreal extent = std::min(bounds.width(), bounds.height());
    const qreal stroke = std::max<qreal>(1.25, extent * 0.105);
    const QSizeF ringSize(extent * 0.58, extent * 0.29);

    QPen pen = painter.pen();
    pen.setWidthF(stroke);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QPointF center = bounds.center();
    const qreal offset = extent * 0.17;
    const auto drawRing = [&](const QPointF& ringCenter) {
        painter.save();
        painter.translate(ringCenter);
        painter.rotate(-45.0);
        const QRectF ringRect(
            -ringSize.width() / 2.0,
            -ringSize.height() / 2.0,
            ringSize.width(),
            ringSize.height());
        const qreal radius = ringSize.height() / 2.0;
        painter.drawRoundedRect(ringRect, radius, radius);
        painter.restore();
    };

    drawRing(center + QPointF(-offset, offset));
    drawRing(center + QPointF(offset, -offset));
}

void drawBulletListGlyph(QPainter& painter, const QRectF& bounds)
{
    const qreal extent = std::min(bounds.width(), bounds.height());
    const qreal stroke = std::max<qreal>(1.2, extent * 0.095);
    const qreal radius = std::max<qreal>(1.1, extent * 0.075);
    const qreal dotX = bounds.left() + extent * 0.14;
    const qreal lineLeft = bounds.left() + extent * 0.34;
    const qreal lineRight = bounds.left() + extent * 0.96;

    QPen pen = painter.pen();
    pen.setWidthF(stroke);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(painter.pen().color());

    for (qreal fraction : {0.22, 0.50, 0.78}) {
        const qreal y = bounds.top() + extent * fraction;
        painter.drawEllipse(QPointF(dotX, y), radius, radius);
        painter.drawLine(QPointF(lineLeft, y), QPointF(lineRight, y));
    }
}

void drawNumberedListGlyph(QPainter& painter, const QRectF& bounds,
                           const QFont& baseFont)
{
    const qreal extent = std::min(bounds.width(), bounds.height());
    const qreal stroke = std::max<qreal>(1.2, extent * 0.09);
    const qreal numberWidth = extent * 0.27;
    const qreal lineLeft = bounds.left() + extent * 0.38;
    const qreal lineRight = bounds.left() + extent * 0.96;

    QPen pen = painter.pen();
    pen.setWidthF(stroke);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    QFont numberFont = baseFont;
    numberFont.setBold(true);
    numberFont.setPixelSize(std::max(7, qRound(extent * 0.31)));

    const QFont savedFont = painter.font();
    for (int index = 0; index < 3; ++index) {
        const qreal fraction = 0.22 + 0.28 * index;
        const qreal y = bounds.top() + extent * fraction;
        const QRectF numberRect(
            bounds.left(),
            y - extent * 0.17,
            numberWidth,
            extent * 0.34);
        painter.setFont(numberFont);
        painter.drawText(numberRect,
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(index + 1));
        painter.drawLine(QPointF(lineLeft, y), QPointF(lineRight, y));
    }
    painter.setFont(savedFont);
}

void drawPriorityGlyph(QPainter& painter, const QRectF& bounds)
{
    const qreal extent = std::min(bounds.width(), bounds.height());
    const qreal stroke = std::max<qreal>(1.25, extent * 0.10);
    const QPointF center = bounds.center();
    const qreal radius = extent * 0.43;

    QPen pen = painter.pen();
    pen.setWidthF(stroke);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, radius, radius);

    painter.drawLine(
        QPointF(center.x(), center.y() - extent * 0.23),
        QPointF(center.x(), center.y() + extent * 0.07));
    painter.setPen(Qt::NoPen);
    painter.setBrush(pen.color());
    painter.drawEllipse(
        QPointF(center.x(), center.y() + extent * 0.25),
        std::max<qreal>(1.0, extent * 0.055),
        std::max<qreal>(1.0, extent * 0.055));
}

void drawFormattingToolbarIcon(QPainter& painter,
                               const QWidget& widget,
                               const QString& objectName,
                               const QColor& color)
{
    const qreal extent = formattingToolbarOpticalExtent(widget);
    const QRectF bounds(
        (widget.width() - extent) / 2.0,
        (widget.height() - extent) / 2.0,
        extent,
        extent);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(color);
    painter.setBrush(color);

    if (objectName == QStringLiteral("formatLinkButton")) {
        drawLinkGlyph(painter, bounds);
    } else if (objectName == QStringLiteral("formatBulletListButton")) {
        drawBulletListGlyph(painter, bounds);
    } else if (objectName == QStringLiteral("formatNumberedListButton")) {
        drawNumberedListGlyph(painter, bounds, widget.font());
    } else if (objectName == QStringLiteral("messagePriorityButton")) {
        drawPriorityGlyph(painter, bounds);
    }

    painter.restore();
}

} // namespace

ThemeIconButton::ThemeIconButton(QWidget* parent)
    : QPushButton(parent)
{
    setCursor(Qt::PointingHandCursor);
}

QString ThemeIconButton::symbolicResource() const
{
    const QString configuredResource = property(ThemeIconResourceProperty).toString();
    if (!configuredResource.isEmpty()) {
        return configuredResource;
    }
    if (objectName() == QStringLiteral("addEmojiButton")) {
        return QStringLiteral(":/icons/emoji");
    }
    if (objectName() == QStringLiteral("attachButton")) {
        return QStringLiteral(":/icons/paperclip");
    }
    return {};
}

bool ThemeIconButton::isBusy() const
{
    if (property(ThemeIconBusyProperty).toBool()) {
        return true;
    }
    return objectName() == QStringLiteral("attachButton")
        && (!property(ComposerBusyTextProperty).toString().isEmpty()
            || property(ComposerMessageLoadingProperty).toBool());
}

void ThemeIconButton::syncBusyAnimation()
{
    const bool busy = isBusy();
    if (busy == _busyAnimationAcquired) {
        update();
        return;
    }

    _busyAnimationAcquired = busy;
    if (busy) {
        BusyIndicator::Animation::instance().acquire(*this);
    } else {
        BusyIndicator::Animation::instance().release(*this);
    }
    update();
}

void ThemeIconButton::invalidateRenderedIcon()
{
    _renderedTint.clear();
    _renderedResource.clear();
    _renderedSize = {};
    _renderedPixmap = {};
}

bool ThemeIconButton::event(QEvent* event)
{
    const QEvent::Type type = event ? event->type() : QEvent::None;
    const bool result = QPushButton::event(event);

    if (type == QEvent::DynamicPropertyChange) {
        invalidateRenderedIcon();
        syncBusyAnimation();
    } else if (type == QEvent::PaletteChange
               || type == QEvent::ApplicationPaletteChange
               || type == QEvent::StyleChange
               || type == QEvent::FontChange
               || type == QEvent::ScreenChangeInternal) {
        invalidateRenderedIcon();
        if (isFormattingToolbar(parentWidget())) {
            syncFormattingToolbarButtonGeometry(parentWidget());
        }
        update();
    } else if (type == QEvent::Enter
               || type == QEvent::Leave
               || type == QEvent::EnabledChange) {
        update();
    }
    return result;
}

void ThemeIconButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    if (isFormattingToolbar(parentWidget())) {
        syncFormattingToolbarButtonGeometry(parentWidget());
    }

    const QPalette currentPalette = qApp ? qApp->palette() : palette();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (isBusy()) {
        QColor busyColor = currentPalette.color(QPalette::WindowText);
        busyColor.setAlpha(190);
        const int indicatorExtent = BusyIndicatorExtent - 5;
        const QPixmap frame = BusyIndicator::Animation::instance().frame(
            indicatorExtent, busyColor, 2.0, devicePixelRatioF());
        painter.drawPixmap(
            QPointF((width() - indicatorExtent) / 2.0 + 2.5,
                    (height() - indicatorExtent) / 2.0 + 2.5),
            frame);
        return;
    }

    const QPalette::ColorGroup group = isEnabled()
        ? QPalette::Active : QPalette::Disabled;
    QColor color = isChecked() && isEnabled()
        ? currentPalette.color(QPalette::Highlight)
        : currentPalette.color(group, QPalette::ButtonText);
    if (!underMouse() && !isChecked()) {
        color.setAlphaF(color.alphaF() * RestingOpacity);
    }

    if (isFormattingToolbarIcon(objectName())) {
        drawFormattingToolbarIcon(painter, *this, objectName(), color);
        return;
    }

    const QString resource = symbolicResource();
    if (!resource.isEmpty()) {
        const QSize targetSize = iconSize().isValid() ? iconSize() : QSize(24, 24);
        const QString desiredTint = tintKey(color);
        if (_renderedTint != desiredTint
            || _renderedResource != resource
            || _renderedSize != targetSize) {
            _renderedPixmap =
                IconUtils::tintedSymbolicIcon(resource, color).pixmap(targetSize);
            _renderedTint = desiredTint;
            _renderedResource = resource;
            _renderedSize = targetSize;
        }

        if (!_renderedPixmap.isNull()) {
            const qreal dpr = std::max<qreal>(
                1.0, _renderedPixmap.devicePixelRatioF());
            const QSizeF logicalSize(
                _renderedPixmap.width() / dpr,
                _renderedPixmap.height() / dpr);
            const QPointF topLeft(
                (width() - logicalSize.width()) / 2.0,
                (height() - logicalSize.height()) / 2.0);
            painter.drawPixmap(topLeft, _renderedPixmap);
        }
        return;
    }

    painter.setPen(color);
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text());
}

} // namespace Mattermost
