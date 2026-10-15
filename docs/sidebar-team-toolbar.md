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
The right-hand hashtag icon opens Browse public channels directly;
the 20px symbolic plus is identical to the sidebar category plus and joins a
new team. No team dropdown or overflow action menu is present.

A separate avatar menu manages user availability and custom status.

Team pills and the button row are explicitly left-aligned, so a single team
starts at the sidebar tree edge instead of floating in the center of the row.
