#include "FormattingToolbarOverlay.h"

#include <algorithm>

#include <QEvent>
#include <QAbstractScrollArea>
#include <QCursor>
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
    // Match the old toolbar's chat background instead of a white Base card.
    // Stay opaque to prevent message text showing through the controls.
    _panel->setFrameShape(QFrame::NoFrame);
    _panel->setBackgroundRole(QPalette::Window);
    _panel->setAutoFillBackground(true);

    auto* panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
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
            _panelHovered = false;
        }
    });

    _hideTimer = new QTimer(this);
    _hideTimer->setSingleShot(true);
    _hideTimer->setInterval(75);
    connect(_hideTimer, &QTimer::timeout, this, [this] {
        if (!_editorHovered && !_panelHovered) {
            animateExpanded(false);
        }
    });
    // Enter/Leave is not always delivered to child widgets when a cursor
    // exits the application directly across the native window boundary.
    // While the overlay is visible, independently check that the pointer
    // remains within ChatArea. No polling takes place while it is hidden.
    _boundaryTimer = new QTimer(this);
    _boundaryTimer->setInterval(200);
    connect(_boundaryTimer, &QTimer::timeout, this, [this] {
        if (_expanded && _host
            && !_host->rect().contains(_host->mapFromGlobal(QCursor::pos()))) {
            clearHoverOnWindowExit();
        }
    });

    host.installEventFilter(this);
    editor.installEventFilter(this);
    if (auto* scroll = qobject_cast<QAbstractScrollArea*>(&editor)) {
        _editorViewport = scroll->viewport();
        _editorViewport->installEventFilter(this);
    }
    _panel->installEventFilter(this);
    toolbar.installEventFilter(this);

    // The first FlowLayout polish pass can occur after constructing the
    // ChatArea. Recompute once that pass and the editor layout have settled.
    QTimer::singleShot(0, this, &FormattingToolbarOverlay::refreshGeometry);
}

QWidget* FormattingToolbarOverlay::panel() const
{
    return _panel;
}

void FormattingToolbarOverlay::setExpanded(bool focused)
{
    _focusRequested = focused;
    updateHoverVisibility();
}

void FormattingToolbarOverlay::updateHoverVisibility()
{
    if (_focusRequested && (_editorHovered || _panelHovered)) {
        _hideTimer->stop();
        animateExpanded(true);
    } else if (_focusRequested && _expanded) {
        // Brief grace period for crossing the editor-to-panel gap.
        _hideTimer->start();
    } else {
        _hideTimer->stop();
        animateExpanded(false);
    }
}

void FormattingToolbarOverlay::clearHoverOnWindowExit()
{
    // A native window leave need not produce Leave for the QTextEdit viewport.
    // Clear both cached flags: otherwise stale _editorHovered=true would
    // immediately resurrect the bar at the next focus/geometry event.
    _editorHovered = false;
    _panelHovered = false;
    _hideTimer->stop();
    animateExpanded(false);
}

void FormattingToolbarOverlay::animateExpanded(bool expanded)
{
    if (_expanded == expanded || !_panel) {
        return;
    }
    _expanded = expanded;

    if (expanded) {
        _boundaryTimer->start();
    } else {
        _boundaryTimer->stop();
    }
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
    if (watched == _host
        && (type == QEvent::Leave || type == QEvent::Hide
            || type == QEvent::WindowDeactivate)) {
        clearHoverOnWindowExit();
    }
    const bool editorSurface = watched == _editor || watched == _editorViewport;
    const bool toolbarSurface = watched == _panel || watched == _toolbar;
    if ((editorSurface || toolbarSurface)
        && (type == QEvent::Enter || type == QEvent::Leave
            || type == QEvent::MouseButtonPress)) {
        const bool entering = type != QEvent::Leave;
        if (editorSurface) _editorHovered = entering;
        else _panelHovered = entering;
        updateHoverVisibility();
    }
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
    // Attachment and send buttons inset the editor, but not the chat.
    // Cover the entire chat width without arbitrary horizontal gutters.
    const int width = std::max(1, _host->width());
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
    const QRect target(0,
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
