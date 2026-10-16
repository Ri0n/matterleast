#pragma once

#include <QObject>
#include <QPointer>

class QEvent;
class QFrame;
class QGraphicsOpacityEffect;
class QVariantAnimation;
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
                             QWidget& toolbar);
    QWidget* panel() const;

public slots:
    void setExpanded(bool expanded);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void refreshGeometry();
    void applyProgress(qreal progress);

    QPointer<QWidget> _host;
    QPointer<QWidget> _editor;
    QPointer<QWidget> _toolbar;
    QFrame* _panel = nullptr;
    QGraphicsOpacityEffect* _opacity = nullptr;
    QVariantAnimation* _animation = nullptr;
    qreal _progress = 0.0;
    bool _expanded = false;
};

} // namespace Mattermost
