# One-click team toolbar

The sidebar renders a fixed horizontal row of team buttons directly below
the user header (name/presence) and above the channel filter. Teams with
actual ChannelTree roots are displayed in alphabetical order, with elided
names and full-name tooltips. The selected team uses a native checked/sunken
QToolButton whose Button palette matches the sidebar background. The \`+\`
button invokes the existing team-join path; \`…\` retains access to Browse
public channels for the selected team.

The hidden legacy TeamSelectorLabel stays alive to own the active team ID,
persistence in \`sidebar/active_team_id\`, and ChannelTree root-index
switching. Do not replicate this state elsewhere. Changes from navigation
or joining teams must update the strip, and root removal must not leave
stale buttons. Widgets removed during a click are released via
\`deleteLater()\` to avoid deleting their own signal emitter.

The button strip scrolls horizontally when teams exceed its viewport.
