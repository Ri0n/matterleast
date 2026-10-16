#include "FormattingToolbarOverlay.h"

#include <algorithm>

#include <QEvent>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QLayout>
#include <QMargins>
#include <QPalette>
#include <QVBoxLayout>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

namespace Mattermost {
namespace {

constexpr int AnimationDurationMs = 110;
constexpr int OverlayGap = 3;
constexpr int SlideDistance = 7;
constexpr int BorderPaddingHorizontal = 4;
constexpr int BorderPaddingVertical = 3;

} // namespace

FormattingToolbarOverlay::FormattingToolbarOverlay(
    QWidget& host, QWidget& editor, QWidget& toolbar)
    : QObject(&host)
    , _host(&host)
    , _editor(&editor)
    , _toolbar(&toolbar)
{
    // Remove the toolbar from the document footer layout before reparenting.
    // Only the editor stays in the layout, so the log never inherits the
    // changing height of the formatting controls.
    if (QWidget* oldParent = toolbar.parentWidget()) {
        if (QLayout* layout = oldParent->layout()) {
            layout->removeWidget(&toolbar);
        }
    }

    _panel = new QFrame(&host);
    _panel->setObjectName(QStringLiteral("formattingToolbarOverlay"));
    _panel->setFrameShape(QFrame::StyledPanel);
    _panel->setBackgroundRole(QPalette::Base);
    _panel->setAutoFillBackground(true);

    auto* panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(
        BorderPaddingHorizontal, BorderPaddingVertical,
        BorderPaddingHorizontal, BorderPaddingVertical);
    panelLayout->setSpacing(0);

    toolbar.setParent(_panel);
    toolbar.setMinimumHeight(0);
    toolbar.setMaximumHeight(QWIDGETSIZE_MAX);
    // The standard toolbar may wrap onto multiple rows (FlowLayout). Its
    // bottom padding separates the lowest buttons from the panel frame.
    if (QLayout* contentLayout = toolbar.layout()) {
        QMargins margins = contentLayout->contentsMargins();
        margins.setBottom(std::max(margins.bottom(), 2));
        contentLayout->setContentsMargins(margins);
    }
    panelLayout->addWidget(&toolbar);
    toolbar.show();

    _opacity = new QGraphicsOpacityEffect(_panel);
    _opacity->setOpacity(0.0);
    _panel->setGraphicsEffect(_opacity);
    _panel->hide();

    _animation = new QVariantAnimation(this);
    _animation->setDuration(AnimationDurationMs);
    connect(_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
        applyProgress(value.toReal());
    });
    connect(_animation, &QVariantAnimation::finished, this, [this] {
        if (!_expanded && _panel) {
            _panel->hide();
        }
    });

    host.installEventFilter(this);
    editor.installEventFilter(this);
    toolbar.installEventFilter(this);

    // The first FlowLayout polish pass can occur after constructing the
    // ChatArea. Recompute once that pass and the editor layout have settled.
    QTimer::singleShot(0, this, &FormattingToolbarOverlay::refreshGeometry);
}

QWidget* FormattingToolbarOverlay::panel() const
{
    return _panel;
}

void FormattingToolbarOverlay::setExpanded(bool expanded)
{
    if (_expanded == expanded || !_panel) {
        return;
    }
    _expanded = expanded;

    _animation->stop();
    if (expanded) {
        refreshGeometry();
        _panel->show();
        _panel->raise();
    }

    if (qFuzzyCompare(_progress + 1.0, (expanded ? 1.0 : 0.0) + 1.0)) {
        applyProgress(expanded ? 1.0 : 0.0);
        if (!expanded) {
            _panel->hide();
        }
        return;
    }

    _animation->setStartValue(_progress);
    _animation->setEndValue(expanded ? 1.0 : 0.0);
    _animation->setEasingCurve(expanded
        ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    _animation->start();
}

bool FormattingToolbarOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return QObject::eventFilter(watched, event);
    }
    const auto type = event->type();
    if ((watched == _host || watched == _editor)
        && (type == QEvent::Resize || type == QEvent::Move
            || type == QEvent::Show)) {
        refreshGeometry();
    } else if (watched == _toolbar
               && (type == QEvent::LayoutRequest || type == QEvent::FontChange
                   || type == QEvent::StyleChange)) {
        // LayoutRequest is queued after changes to button metrics or width.
        // Geometry is outside any layout and may be recomputed independently.
        refreshGeometry();
    }
    return QObject::eventFilter(watched, event);
}

void FormattingToolbarOverlay::refreshGeometry()
{
    if (!_panel || !_host || !_editor || !_toolbar) {
        return;
    }

    const QPoint editorTop = _editor->mapTo(_host, QPoint(0, 0));
    const int width = std::max(1, _editor->width());
    const QMargins margins = _panel->layout()->contentsMargins();
    const int contentWidth = std::max(
        1, width - margins.left() - margins.right() - 2 * _panel->frameWidth());
    QLayout* contentLayout = _toolbar->layout();
    if (!contentLayout) {
        return;
    }

    const int contentHeight = contentLayout->hasHeightForWidth()
        ? contentLayout->heightForWidth(contentWidth)
        : contentLayout->sizeHint().height();
    const int height = std::max(1, contentHeight + margins.top()
        + margins.bottom() + 2 * _panel->frameWidth());
    const int shift = qRound((1.0 - _progress) * SlideDistance);
    const QRect target(editorTop.x(),
                       editorTop.y() - height - OverlayGap + shift,
                       width, height);
    if (_panel->geometry() != target) {
        _panel->setGeometry(target);
    }
}

void FormattingToolbarOverlay::applyProgress(qreal progress)
{
    _progress = qBound<qreal>(0.0, progress, 1.0);
    if (_opacity) {
        _opacity->setOpacity(_progress);
    }
    refreshGeometry();
}

} // namespace Mattermost
