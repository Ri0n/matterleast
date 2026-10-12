# Restore only visible thread surfaces

MatterLeast saves navigation tabs as semantic channel/root/post destinations,
not as an instruction to eagerly build a ChatArea for each tab. At application
startup, restore the tab bar and pinned flags immediately, and materialize only
the selected tab. Switching to an inactive tab constructs it on demand,
restoring the saved post ID using the existing semantic viewport bookmark API.

The docked thread pane has different semantics: QStackedWidget historically
retained old ChatArea children for every thread ever opened. Only its **visible
current widget** represents an open dock. Save that dock exclusively, and on
restoration ignore stale dock records in older session version 1 snapshots.
Detached **visible** windows are independent surfaces and still restore.

Do not change `setMaterializationLimit(200)` to hide eager view creation.
A future refinement will persist a fractional viewport anchor alongside post
identity rather than implicitly recentering bookmarked posts.

Regression: `ThreadTabsIntegrationTest::startupOnlyMaterializesSelectedTabAndVisibleDock`.
