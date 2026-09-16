from pathlib import Path
root = Path(__file__).resolve().parents[2]
checks = {
    'sources/channel-tree/ChannelTree.cpp': [
        'verifySidebarTeam(teamId, mutation)',
        'reconcileTeamSidebar(*backendForSidebar',
    ],
    'sources/channel-tree/ChannelTreeDragVisuals.cpp': [
        'DragAnimationMs = 160',
        'QEasingCurve::OutCubic',
        'currentDragGapExtent',
    ],
    'sources/channel-tree/ChannelTreeReconcile.cpp': [
        'destroySidebarRow',
        'movableChannels',
    ],
}
for path, needles in checks.items():
    text = (root / path).read_text()
    for needle in needles:
        if needle not in text:
            raise SystemExit(f'{needle!r} missing in {path}')

# Guard against the exact accidental mechanical replacement caught during review.
channel_tree = (root / 'sources/channel-tree/ChannelTree.cpp').read_text()
create_group = channel_tree[channel_tree.index('void ChannelTree::createGroupAndMoveChannel'):
                            channel_tree.index('void ChannelTree::moveChannelToCategory')]
if 'mutation' in create_group:
    raise SystemExit('createGroupAndMoveChannel unexpectedly references DnD mutation generation')
print('sidebar DnD source sanity OK')
