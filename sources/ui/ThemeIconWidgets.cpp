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
#include <QLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPalette>
#include <QSizePolicy>
#include <QVector>
#include <QWidget>

#include "BusyIndicator.h"
#include "FlowLayout.h"
#include "IconUtils.h"
#include "SvgRasterCache.h"

namespace Mattermost {
namespace {

constexpr qreal RestingOpacity = 0.8;
constexpr int BusyIndicatorExtent = 18;
constexpr qreal FormattingToolbarOpticalScale = 1.28;
constexpr int FormattingToolbarButtonPadding = 8;
constexpr int FormattingToolbarHorizontalPadding = 10;
constexpr char FormattingToolbarFlowProperty[] =
    "_matterleast_formatting_toolbar_flow";

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

bool isFormattingToolbarIcon(const QString& objectName)
{
    return objectName == QStringLiteral("formatLinkButton")
        || objectName == QStringLiteral("formatImageButton")
        || objectName == QStringLiteral("formatBulletListButton")
        || objectName == QStringLiteral("formatNumberedListButton")
        || objectName == QStringLiteral("messagePriorityButton");
}

bool isFormattingToolbar(const QWidget* widget)
{
    return widget && widget->objectName() == QStringLiteral("formattingToolbar");
}

void ensureFormattingToolbarFlowLayout(QWidget* toolbar)
{
    if (!isFormattingToolbar(toolbar)
        || toolbar->property(FormattingToolbarFlowProperty).toBool()) {
        return;
    }

    QLayout* oldLayout = toolbar->layout();
    if (!oldLayout) {
        return;
    }

    // A fixed QHBoxLayout cannot satisfy all fixed-size formatting buttons once
    // the composer gets narrower than their combined width. Preserve the same
    // button order but let the already shared FlowLayout wrap whole buttons onto
    // another row instead of squeezing their geometries into each other.
    QVector<QLayoutItem*> buttonItems;
    while (QLayoutItem* item = oldLayout->takeAt(0)) {
        if (item->widget()) {
            buttonItems.push_back(item);
        } else {
            // The horizontal layout may contain a trailing stretch/spacer. It
            // has no meaning in a wrapping layout and would create blank rows.
            delete item;
        }
    }

    const QString layoutName = oldLayout->objectName();
    delete oldLayout;

    auto* flow = new FlowLayout(toolbar, 0, 1);
    flow->setObjectName(layoutName);
    for (QLayoutItem* item : buttonItems) {
        flow->addItem(item);
    }

    QSizePolicy policy = toolbar->sizePolicy();
    policy.setVerticalPolicy(QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    toolbar->setSizePolicy(policy);
    toolbar->setProperty(FormattingToolbarFlowProperty, true);
    toolbar->updateGeometry();
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

void syncFormattingButtonFont(QAbstractButton* button, const QFont& toolbarFont)
{
    if (!button) {
        return;
    }

    // Individual toolbar buttons carry local traits (bold/italic/strike), but
    // their size must follow the toolbar font. Designer-local fonts otherwise
    // stop inheriting application font scaling and drift away from SVG icons.
    QFont font = button->font();
    if (toolbarFont.pointSizeF() > 0.0) {
        font.setPointSizeF(toolbarFont.pointSizeF());
    } else if (toolbarFont.pixelSize() > 0) {
        font.setPixelSize(toolbarFont.pixelSize());
    }
    if (button->font() != font) {
        button->setFont(font);
    }
}

void syncFormattingToolbarButtonGeometry(QWidget* toolbar)
{
    if (!isFormattingToolbar(toolbar)) {
        return;
    }

    const QFont toolbarFont = toolbar->font();
    const QFontMetrics metrics(toolbarFont);
    const int buttonHeight = std::max(
        28, metrics.height() + FormattingToolbarButtonPadding);
    bool geometryChanged = false;

    const auto buttons = toolbar->findChildren<QAbstractButton*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QAbstractButton* button : buttons) {
        if (!button) {
            continue;
        }

        syncFormattingButtonFont(button, toolbarFont);

        int buttonWidth = buttonHeight;
        if (!button->text().isEmpty()) {
            const QFontMetrics buttonMetrics(button->font());
            buttonWidth = std::max(
                buttonHeight,
                buttonMetrics.horizontalAdvance(button->text())
                    + FormattingToolbarHorizontalPadding);
        }

        const QSize target(buttonWidth, buttonHeight);
        if (button->minimumSize() != target
            || button->maximumSize() != target) {
            button->setMinimumSize(target);
            button->setMaximumSize(target);
            button->updateGeometry();
            geometryChanged = true;
        }
    }

    if (geometryChanged && toolbar->layout()) {
        toolbar->layout()->invalidate();
        toolbar->updateGeometry();
    }
}

void tintPixmap(QPixmap& pixmap, const QColor& color)
{
    if (pixmap.isNull()) {
        return;
    }

    const qreal dpr = std::max<qreal>(1.0, pixmap.devicePixelRatioF());
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(
        QRectF(0.0,
               0.0,
               pixmap.width() / dpr,
               pixmap.height() / dpr),
        color);
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
    if (objectName() == QStringLiteral("formatImageButton")) {
        return QStringLiteral(":/icons/image");
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
    _renderedDprMilli = 0;
}

bool ThemeIconButton::event(QEvent* event)
{
    const QEvent::Type type = event ? event->type() : QEvent::None;
    const bool result = QPushButton::event(event);

    if ((type == QEvent::Polish || type == QEvent::ShowToParent
         || type == QEvent::ParentChange)
        && isFormattingToolbar(parentWidget())) {
        ensureFormattingToolbarFlowLayout(parentWidget());
        syncFormattingToolbarButtonGeometry(parentWidget());
    }

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
        const qreal currentDpr = std::max<qreal>(1.0, devicePixelRatioF());
        const int currentDprMilli = qRound(currentDpr * 1000.0);
        const QString desiredTint = tintKey(color);
        if (_renderedTint != desiredTint
            || _renderedResource != resource
            || _renderedSize != targetSize
            || _renderedDprMilli != currentDprMilli) {
            if (formattingIcon) {
                _renderedPixmap = SvgRasterCache::instance().raster(
                    resource, targetSize, currentDpr);
                tintPixmap(_renderedPixmap, color);
            } else {
                _renderedPixmap =
                    IconUtils::tintedSymbolicIcon(resource, color).pixmap(targetSize);
            }
            _renderedTint = desiredTint;
            _renderedResource = resource;
            _renderedSize = targetSize;
            _renderedDprMilli = currentDprMilli;
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
