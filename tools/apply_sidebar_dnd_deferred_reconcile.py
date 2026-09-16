from pathlib import Path

header = Path('sources/channel-tree/ChannelTree.h')
text = header.read_text()
old = '''    void verifySidebarTeam(const QString& teamId, quint64 mutation);\n    void refreshPaletteDependentIcons();\n'''
new = '''    void verifySidebarTeam(const QString& teamId, quint64 mutation);\n    void flushDeferredSidebarReconciles();\n    void refreshPaletteDependentIcons();\n'''
assert old in text
text = text.replace(old, new, 1)
old = '''    QVector<CategoryDragBoundary>       categoryDragBoundaries;\n    QMap<QString, quint64>              sidebarMutationGeneration;\n'''
new = '''    QVector<CategoryDragBoundary>       categoryDragBoundaries;\n    QSet<QString>                       pendingSidebarReconcileTeams;\n    QMap<QString, quint64>              sidebarMutationGeneration;\n'''
assert old in text
header.write_text(text.replace(old, new, 1))

reconcile = Path('sources/channel-tree/ChannelTreeReconcile.cpp')
text = reconcile.read_text()
old = '''void ChannelTree::reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,\n                                       const SidebarTeamState& state)\n{\n    // A server reconciliation is structural. Never leave transient drag geometry\n    // attached to rows that may be moved or removed underneath it.\n    resetDragVisuals(false);\n    renderingSidebar = true;\n'''
new = '''void ChannelTree::reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,\n                                       const SidebarTeamState& state)\n{\n    // A live QDrag owns the tree structure until it finishes. Reconciliation\n    // during a drag would unhide/reparent the real source item underneath the\n    // drag pixmap, producing a visible duplicate and invalidating gap anchors.\n    // SidebarService still receives network state normally; coalesce structural\n    // updates per team and apply only the newest state once the drag finishes.\n    if (!dragSourceIndexes.isEmpty()) {\n        pendingSidebarReconcileTeams.insert(teamItem.teamId);\n        return;\n    }\n\n    resetDragVisuals(false);\n    renderingSidebar = true;\n'''
assert old in text
text = text.replace(old, new, 1)
insert_before = '''void ChannelTree::destroySidebarRow(QTreeWidgetItem* item)\n'''
flush = '''void ChannelTree::flushDeferredSidebarReconciles()\n{\n    if (!backendForSidebar || !dragSourceIndexes.isEmpty()\n        || pendingSidebarReconcileTeams.isEmpty()) {\n        return;\n    }\n\n    const QSet<QString> pending = pendingSidebarReconcileTeams;\n    pendingSidebarReconcileTeams.clear();\n\n    auto& sidebar = SidebarService::instance(*backendForSidebar);\n    for (const QString& teamId : pending) {\n        TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);\n        const SidebarTeamState* state = sidebar.teamState(teamId);\n        if (!teamItem || !state) {\n            continue;\n        }\n        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);\n    }\n}\n\n'''
assert insert_before in text
text = text.replace(insert_before, flush + insert_before, 1)
reconcile.write_text(text)

drag = Path('sources/channel-tree/ChannelTreeDragVisuals.cpp')
text = drag.read_text()
old = '''    drag.exec(Qt::MoveAction, Qt::MoveAction);\n    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {\n        resetDragVisuals(false);\n    }\n}\n'''
new = '''    drag.exec(Qt::MoveAction, Qt::MoveAction);\n    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {\n        resetDragVisuals(false);\n    }\n    flushDeferredSidebarReconciles();\n}\n'''
assert old in text
text = text.replace(old, new, 1)
drag.write_text(text)
