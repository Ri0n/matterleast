#pragma once

#include <functional>
#include <utility>

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
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
 * The deferred application-palette refresh runs after Qt has propagated the
 * new palette to child widgets; it also covers custom native platform styles
 * that do not send PaletteChange to every child consistently.
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
        if (qApp) {
            connect(qApp, &QGuiApplication::paletteChanged, this,
                    [this](const QPalette&) {
                if (_refreshScheduled) {
                    return;
                }
                _refreshScheduled = true;
                QTimer::singleShot(0, this, [this] {
                    _refreshScheduled = false;
                    refreshIcon();
                });
            });
        }
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
