#pragma once

#include <QtPlugin>

class QWidget;

namespace MatterLeast::PluginApi {

/**
 * Optional plugin extension for a passive overlay on MatterLeast's main window.
 *
 * The host passes only a Qt QWidget pointer, never an application-specific
 * MainWindow type. The plugin owns its overlay implementation and must delete
 * its child widget and remove any event filters in disable(), before unload.
 * attachToMainWindow() may be called again when a disabled plugin is re-enabled.
 */
class MainWindowOverlayInterface
{
public:
    virtual ~MainWindowOverlayInterface() = default;

    virtual void attachToMainWindow(QWidget* window) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_MAIN_WINDOW_OVERLAY_IID \
    "io.github.Ri0n.MatterLeast.MainWindowOverlay/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::MainWindowOverlayInterface,
                    MATTERLEAST_MAIN_WINDOW_OVERLAY_IID)
