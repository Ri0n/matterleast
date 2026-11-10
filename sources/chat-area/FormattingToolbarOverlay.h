#pragma once

#include <QObject>
#include <QPointer>

class QEvent;
class QFrame;
class QHBoxLayout;
class QVBoxLayout;
class QGraphicsOpacityEffect;
class QLayout;
class QVariantAnimation;
class QTimer;
class QWidget;

namespace Mattermost {

// Owns the formatting bar as an overlay above the composer. Unlike a toolbar
// in the footer's QVBoxLayout, this does not alter the chat log viewport when
// the editor gains or loses focus. The editor's original focus policy remains
// the single visibility authority.
class FormattingToolbarOverlay final : public QObject
{
    Q_OBJECT
public:
    FormattingToolbarOverlay(QWidget& host, QWidget& editor,
                             QWidget& toolbar, QLayout& footerLayout);
    QWidget* panel() const;
    void installPreview(QWidget& preview, QWidget& toggleButton);
    void setPreviewVisible(bool visible);

public slots:
    void setExpanded(bool expanded);

private:
    void animateExpanded(bool expanded);
    void updateHoverVisibility();
    void clearHoverOnWindowExit();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void refreshGeometry();
    void scheduleGeometryRefresh();
    int composerTop() const;
    void applyProgress(qreal progress);

    QPointer<QWidget> _host;
    QPointer<QWidget> _editor;
    QPointer<QLayout> _footerLayout;
    QPointer<QWidget> _footerOwner;
    QPointer<QWidget> _toolbar;
    QPointer<QWidget> _editorViewport;
    QPointer<QWidget> _preview;
    QPointer<QWidget> _previewToggle;
    QVBoxLayout* _panelLayout = nullptr;
    QHBoxLayout* _toolbarRow = nullptr;
    QTimer* _hideTimer = nullptr;
    QTimer* _boundaryTimer = nullptr;
    QFrame* _panel = nullptr;
    QGraphicsOpacityEffect* _opacity = nullptr;
    QVariantAnimation* _animation = nullptr;
    qreal _progress = 0.0;
    bool _expanded = false;
    bool _focusRequested = false;
    bool _editorHovered = false;
    bool _panelHovered = false;
    bool _previewVisible = false;
    bool _geometryRefreshQueued = false;
};

} // namespace Mattermost
