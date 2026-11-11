#pragma once

#include <functional>
#include <utility>

#include <QEvent>
#include <QIcon>
#include <QPalette>
#include <QTimer>
#include <QToolButton>

namespace Mattermost {

/**
 * Recreates palette-tinted pixmap icons whenever the owning control changes
 * palette, style or checked state. A plain QToolButton stores the initial
 * tinted QIcon forever, even after the OS switches between light/dark themes.
 *
 * A queued refresh runs after Qt has propagated a PaletteChange or
 * ApplicationPaletteChange through the widget hierarchy, avoiding the
 * deprecated QGuiApplication::paletteChanged signal.
 */
class PaletteIconToolButton : public QToolButton
{
public:
    using IconFactory = std::function<QIcon(const QToolButton&)>;

    explicit PaletteIconToolButton(QWidget* parent = nullptr)
        : QToolButton(parent)
    {
        connect(this, &QToolButton::toggled, this,
                [this] { refreshIcon(); });

    }

    void setIconFactory(IconFactory factory)
    {
        _iconFactory = std::move(factory);
        refreshIcon();
    }

    void refreshIcon()
    {
        if (_iconFactory) {
            setIcon(_iconFactory(*this));
        }
    }

protected:
    void changeEvent(QEvent* event) override
    {
        QToolButton::changeEvent(event);
        if (!event) {
            return;
        }
        switch (event->type()) {
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::StyleChange:
        case QEvent::EnabledChange:
            refreshIcon();
            // Qt can send the change notification before children have
            // inherited the newly installed application/style palette.
            // Refresh again at the end of the event turn.
            if (!_refreshScheduled) {
                _refreshScheduled = true;
                QTimer::singleShot(0, this, [this] {
                    _refreshScheduled = false;
                    refreshIcon();
                });
            }
            break;
        default:
            break;
        }
    }

private:
    IconFactory _iconFactory;
    bool _refreshScheduled = false;
};

} // namespace Mattermost
