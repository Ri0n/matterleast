# Channels tab toolbar

TeamSelectorLabel stays alive as the hidden controller for team context,
ChannelTree root-index switching, restoring and persisting team selection.
Its horizontal team strip lives inside channelsPage, immediately beneath
the Channels tab and above ChannelTree. The Following and Attention tabs
have no toolbar and retain their full available height. The unread-only toggle
is the toolbar's leading control and filters ChannelTree only; it never hides
Following or changes Following/Attention contents. The sidebar text search
remains above the tabs and works independently of the unread toggle.

Team names are always shown on clickable buttons. The active team has a
rounded inset/pressed background. Delicate gray dotted separators sit between
names, which are shortened only when exceptionally long (full name in tooltip).
A scroll area with ‹/› controls handles more teams than fit the width.
The right-hand symbolic plus opens a menu for direct/group conversations,
public-channel browsing (including channel creation), and joining another
team. Each action reuses the existing discovery dialog or controller.
The sidebar tabs use native QTabBar layout with icon-sized minimum widths
and elided labels instead of horizontal scrolling.

A separate avatar menu manages user availability and custom status.

Team pills and the button row are explicitly left-aligned, so a single team
starts at the sidebar tree edge instead of floating in the center of the row.

## Palette changes

The channel toolbar uses the same `IconUtils` tinting as other buttons.
Because the tinted icons are pixmaps, `TeamSelectorLabel` recreates the
`+` icon when its detached toolbar's palette or style changes.
The unread-only filter uses the existing `MainWindow::refreshUnreadFilterIcon()`
path, refreshed on the **same toolbar palette event** as `+`.
Its checked-state stylesheet can retain an old button-local foreground palette,
so the filter icon is tinted from the surrounding toolbar's current palette
rather than `unreadFilterButton->palette()`. No team toolbar rebuild is required.
