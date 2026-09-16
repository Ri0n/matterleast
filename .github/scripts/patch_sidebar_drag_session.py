from pathlib import Path


def replace(path, old, new):
    p = Path(path)
    s = p.read_text()
    if old not in s:
        raise SystemExit(f'pattern not found in {path}: {old[:120]!r}')
    p.write_text(s.replace(old, new, 1))

# Header: explicit session lifetime plus deferred structural work queues.
replace('sources/channel-tree/ChannelTree.h',
'''    void flushDeferredSidebarReconciles();
    void refreshPaletteDependentIcons();''',
'''    void flushDeferredSidebarReconciles();
    void refreshPaletteDependentIcons();''')
replace('sources/channel-tree/ChannelTree.h',
'''    bool                                personalUserConnected = false;
    QVariantAnimation*                  sourceCollapseAnimation = nullptr;''',
'''    bool                                personalUserConnected = false;
    // True for the complete nested QDrag::exec() lifetime. Structural sidebar
    // mutations from network/realtime callbacks are deferred while this is set.
    bool                                sidebarDragActive = false;
    QVariantAnimation*                  sourceCollapseAnimation = nullptr;''')
replace('sources/channel-tree/ChannelTree.h',
'''    QSet<QString>                       pendingSidebarReconcileTeams;
    QMap<QString, quint64>              sidebarMutationGeneration;''',
'''    QSet<QString>                       pendingSidebarReconcileTeams;
    QSet<QString>                       pendingSidebarRefreshTeams;
    QSet<QString>                       pendingStoredChannelOpens;
    QMap<QString, quint64>              sidebarMutationGeneration;''')

# Keep the structural freeze for the whole nested drag event loop, not just
# while a particular vector of visual indexes happens to be populated.
replace('sources/channel-tree/ChannelTreeDragVisuals.cpp',
'''    ensureDragSourceVisuals(source);
    dragStartPointerY = cursorInViewport.y();
    drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(false);
    }
    flushDeferredSidebarReconciles();''',
'''    sidebarDragActive = true;
    ensureDragSourceVisuals(source);
    dragStartPointerY = cursorInViewport.y();
    drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(false);
    }
    sidebarDragActive = false;
    flushDeferredSidebarReconciles();''')

# Reconcile is one structural entry point, but the session flag is the contract.
replace('sources/channel-tree/ChannelTreeReconcile.cpp',
'''    if (!backendForSidebar || !dragSourceIndexes.isEmpty()
        || pendingSidebarReconcileTeams.isEmpty()) {
        return;
    }

    const QSet<QString> pending = pendingSidebarReconcileTeams;
    pendingSidebarReconcileTeams.clear();

    auto& sidebar = SidebarService::instance(*backendForSidebar);
    for (const QString& teamId : pending) {
        TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
        const SidebarTeamState* state = sidebar.teamState(teamId);
        if (!teamItem || !state) {
            continue;
        }
        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);
    }''',
'''    if (!backendForSidebar || sidebarDragActive) {
        return;
    }
    if (pendingSidebarReconcileTeams.isEmpty()
        && pendingSidebarRefreshTeams.isEmpty()
        && pendingStoredChannelOpens.isEmpty()) {
        return;
    }

    const QSet<QString> pendingReconciles = pendingSidebarReconcileTeams;
    const QSet<QString> pendingRefreshes = pendingSidebarRefreshTeams;
    const QSet<QString> pendingOpens = pendingStoredChannelOpens;
    pendingSidebarReconcileTeams.clear();
    pendingSidebarRefreshTeams.clear();
    pendingStoredChannelOpens.clear();

    auto& sidebar = SidebarService::instance(*backendForSidebar);
    for (const QString& teamId : pendingReconciles) {
        TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
        const SidebarTeamState* state = sidebar.teamState(teamId);
        if (!teamItem || !state) {
            continue;
        }
        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);
    }

    // A channel/team leave mutates the tree directly in the legacy path. When
    // it occurs during DnD, refresh authoritatively after the drag instead.
    for (const QString& teamId : pendingRefreshes) {
        refreshSidebarTeam(teamId);
    }

    // Programmatic materialization can also create rows directly. Resume it
    // only after the drag-owned tree structure has been released.
    for (const QString& channelId : pendingOpens) {
        openStoredChannel(channelId);
    }''')
replace('sources/channel-tree/ChannelTreeReconcile.cpp',
'''    if (!dragSourceIndexes.isEmpty()) {
        pendingSidebarReconcileTeams.insert(teamItem.teamId);
        return;
    }''',
'''    if (sidebarDragActive) {
        pendingSidebarReconcileTeams.insert(teamItem.teamId);
        return;
    }''')

# Realtime DM/GM admission updates SidebarService immediately, but does not
# materialize a QTreeWidgetItem while a drag owns the tree structure.
replace('sources/channel-tree/ChannelTreeRealtime.cpp',
'''        if (!category->channelIds.contains(channel.id)) {
            category->channelIds.prepend(channel.id);
        }

        QTreeWidgetItem* categoryItem = nullptr;''',
'''        if (!category->channelIds.contains(channel.id)) {
            category->channelIds.prepend(channel.id);
        }

        if (sidebarDragActive) {
            pendingSidebarReconcileTeams.insert(teamIt.key());
            continue;
        }

        QTreeWidgetItem* categoryItem = nullptr;''')

# Programmatic channel materialization is another direct creator. Queue the
# intent instead of changing the tree from a nested network callback mid-drag.
replace('sources/channel-tree/ChannelTreeNavigation.cpp',
'''void ChannelTree::openStoredChannel(QString channelID)
{
    auto existing = channelToItemMap.constFind(channelID);''',
'''void ChannelTree::openStoredChannel(QString channelID)
{
    if (sidebarDragActive) {
        pendingStoredChannelOpens.insert(channelID);
        return;
    }

    auto existing = channelToItemMap.constFind(channelID);''')

# A realtime leave used to delete rows immediately. During a drag defer an
# authoritative sidebar refresh for every affected team instead.
replace('sources/channel-tree/ChannelTree.cpp',
'''    const QString channelId = channel->id;
    const QList<QTreeWidgetItem*> items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {''',
'''    const QString channelId = channel->id;
    const QList<QTreeWidgetItem*> items = channelToItemMap.value(channelId);
    if (sidebarDragActive) {
        for (QTreeWidgetItem* item : items) {
            QTreeWidgetItem* category = item ? item->parent() : nullptr;
            if (!category) {
                continue;
            }
            const QString teamId = category->data(0, ItemTeamIdRole).toString();
            if (!teamId.isEmpty()) {
                pendingSidebarRefreshTeams.insert(teamId);
            }
        }
        return;
    }

    for (QTreeWidgetItem* item : items) {''')

# Team leave is rare but is still structural. Defer the authoritative refresh
# rather than deleting the drag-owned subtree from a realtime callback.
replace('sources/channel-tree/ChannelTree.cpp',
'''\tconnect (&team, &BackendTeam::onLeave, this, [this, &team, teamList] {
        clearTeamSidebar(*teamList);
        teamToItemMap.remove(team.id);''',
'''\tconnect (&team, &BackendTeam::onLeave, this, [this, &team, teamList] {
        if (sidebarDragActive) {
            pendingSidebarRefreshTeams.insert(team.id);
            return;
        }
        clearTeamSidebar(*teamList);
        teamToItemMap.remove(team.id);''')
