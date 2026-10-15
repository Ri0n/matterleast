# Seasonal Snow native plugin

Seasonal Snow is a bundled desktop plugin using the public
`MainWindowOverlayInterface/1.0` capability. It does not depend on
`MainWindow`, `ChatArea`, or any other MatterLeast internal Qt class.

## Activation and timing

The snow effect is enabled automatically by the **local calendar date**
from December 24 through January 7, inclusive. The enabled plugin checks
the date when attached to the main window, then runs a **single-shot**
timer until the next local midnight. Outside the holiday window the
animation timer is completely stopped; there is no polling every frame
for the date. A suspended computer catches up at the next delivered
calendar timer tick.

To preview the effect in development builds outside the holiday period,
start the application with `MATTERLEAST_SNOW_PREVIEW=1`. This is a
temporary process-local test override, not a persistent setting.

## Presentation

The overlay is a transparent `QWidget` child of the main window. It
covers the window client area, including sidebars and current chat panes,
but not separate native popups or detached thread windows.

- `WA_TransparentForMouseEvents` makes the entire layer click-through.
- Sparse flakes (6–22 at 100% intensity, depending on window area) move at
  slightly different speeds (18–34 logical pixels/second) and rotate slowly
  (8–22 degrees/second). The plugin exposes live **Intensity** (25–250%),
  scaling the population with a safety cap.
- **Snowflake size** defaults to 150% of the original 3–6.5 logical-pixel
  radius (resulting radii 4.5–9.75). Users can choose 50–300%. Existing
  flakes resize immediately without restarting or respawning.
- Small translucent flakes and slight sideways oscillation minimize
  interference with chat text. Colors follow the current palette.
- Animation pauses whenever the main window is minimized or hidden,
  and the overlay tracks window resizing.
- Disabling/unloading the plugin deletes the overlay and removes its
  parent event filter before the shared library is unloaded.

Only `MainWindowOverlayInterface` has been added to the public plugin
SDK. The host provides a `QWidget*` for the main window; the extension
owns its visuals, timer and cleanup.

## Distribution

The desktop build generates `matterleast-seasonal-snow` as a native
plugin and installs it into `<application directory>/plugins`, one of
the existing PluginManager discovery locations. Like other native
plugins, it can be turned off or back on under **Settings → Plugins**.
Its settings extension provides snowflake size (default 150%) and intensity
(default 100%) controls. Values are persisted by the host, restored on
re-enable and updated immediately, including outside the active season.
No snow effect is produced on startup outside the season unless preview
mode is explicitly enabled.

The `seasonal-snow-test` tests date boundaries, daily scheduling, a
live plugin instance, click-through behavior, resizing and unload.
