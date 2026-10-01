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
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <QWindow>

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

QPixmap sharpFormattingToolbarPixmap(const QString& resource,
                                      const QColor& color,
                                      const QWidget& widget)
{
    const int logicalExtent = formattingToolbarOpticalExtent(widget);
    const QSize logicalSize(logicalExtent, logicalExtent);
    const QIcon icon(resource);
    if (icon.isNull()) {
        return {};
    }

    QPixmap pixmap;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 has an explicit DPR-aware overload. Keep the requested size in
    // device-independent pixels and let the icon engine rasterize the SVG for
    // the target screen scale. Do not multiply the size by DPR ourselves.
    pixmap = icon.pixmap(logicalSize,
                         std::max<qreal>(1.0, widget.devicePixelRatioF()),
                         QIcon::Normal,
                         QIcon::Off);
#else
    // On Qt 5 the window-aware overload is the equivalent API: it returns a
    // high-DPI pixmap when the target window is on a scaled screen.
    QWindow* windowHandle = widget.window() ? widget.window()->windowHandle()
                                             : nullptr;
    pixmap = windowHandle
        ? icon.pixmap(windowHandle, logicalSize, QIcon::Normal, QIcon::Off)
        : icon.pixmap(logicalSize, QIcon::Normal, QIcon::Off);
#endif
    if (pixmap.isNull()) {
        return {};
    }

    // Tint only; there is deliberately no image scaling after the SVG engine
    // produced the final DPR-aware pixmap.
    QPainter tintPainter(&pixmap);
    tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tintPainter.fillRect(pixmap.rect(), color);
    tintPainter.end();

    return pixmap;
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
    if (objectName() == QStringLiteral("formatLinkButton")) {
        return QStringLiteral(":/icons/link");
    }
    if (objectName() == QStringLiteral("formatBulletListButton")) {
        return QStringLiteral(":/icons/format-bullet-list");
    }
    if (objectName() == QStringLiteral("formatNumberedListButton")) {
        return QStringLiteral(":/icons/format-numbered-list");
    }
    if (objectName() == QStringLiteral("messagePriorityButton")) {
        return QStringLiteral(":/icons/message-priority");
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

    const QString resource = symbolicResource();
    if (!resource.isEmpty()) {
        const bool formattingIcon = isFormattingToolbarIcon(objectName());
        const QSize targetSize = formattingIcon
            ? QSize(formattingToolbarOpticalExtent(*this),
                    formattingToolbarOpticalExtent(*this))
            : (iconSize().isValid() ? iconSize() : QSize(24, 24));
        const QString desiredTint = tintKey(color);
        if (_renderedTint != desiredTint
            || _renderedResource != resource
            || _renderedSize != targetSize) {
            _renderedPixmap = formattingIcon
                ? sharpFormattingToolbarPixmap(resource, color, *this)
                : IconUtils::tintedSymbolicIcon(resource, color).pixmap(targetSize);
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
