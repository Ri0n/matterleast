from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


def write(path, text):
    (ROOT / path).write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if text.count(old) != 1:
        raise RuntimeError(f"expected one anchor in {path}: {old[:100]!r}, got {text.count(old)}")
    write(path, text.replace(old, new, 1))


# QVariant::toDouble is portable across all supported Qt versions.
replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    "index.data(SidebarItem::DragCollapseRole).toReal()",
    "index.data(SidebarItem::DragCollapseRole).toDouble()")

# Track every row that can still carry a non-zero gap. If the pointer moves to a
# third target before the first close animation finishes, the first row must not
# be forgotten with a stale expanded size hint.
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "    QPersistentModelIndex               dragGapIndex;\n",
    "    QVector<QPersistentModelIndex>      dragGapIndexes;\n"
    "    QPersistentModelIndex               currentDragGapIndex;\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid refreshSidebarTeam(const QString& teamId);\n",
    "\tvoid refreshSidebarTeam(const QString& teamId);\n"
    "    void verifySidebarTeam(const QString& teamId, quint64 mutation);\n")

# Fetching for verification must be able to return an authoritative snapshot
# without mutating the optimistic local projection before ChannelTree has checked
# the mutation generation.
replace_once(
    "sources/backend/SidebarService.h",
    "    void retrieveCategories(BackendTeam& team,\n"
    "                            std::function<void(const SidebarTeamState&)> callback = {});",
    "    void retrieveCategories(BackendTeam& team,\n"
    "                            std::function<void(const SidebarTeamState&)> callback = {},\n"
    "                            bool storeResponse = true);")
replace_once(
    "sources/backend/SidebarService.h",
    "    void storeCategories(QString teamId, SidebarTeamState state,\n"
    "                         std::function<void(const SidebarTeamState&)> callback);",
    "    void storeCategories(QString teamId, SidebarTeamState state,\n"
    "                         std::function<void(const SidebarTeamState&)> callback,\n"
    "                         bool storeResponse);")

replace_once(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::storeCategories(QString teamId, SidebarTeamState state,\n"
    "                                     std::function<void(const SidebarTeamState&)> callback)\n",
    "void SidebarService::storeCategories(QString teamId, SidebarTeamState state,\n"
    "                                     std::function<void(const SidebarTeamState&)> callback,\n"
    "                                     bool storeResponse)\n")
replace_once(
    "sources/backend/SidebarService.cpp",
    "                [this, teamId, statePtr, callback] {\n"
    "                    sidebarByTeam.insert(teamId, std::move(*statePtr));\n"
    "                    emit categoriesChanged(teamId);\n"
    "                    if (callback) {\n"
    "                        callback(sidebarByTeam[teamId]);\n"
    "                    }\n"
    "                });",
    "                [this, teamId, statePtr, callback, storeResponse] {\n"
    "                    if (storeResponse) {\n"
    "                        sidebarByTeam.insert(teamId, *statePtr);\n"
    "                        emit categoriesChanged(teamId);\n"
    "                        if (callback) {\n"
    "                            callback(sidebarByTeam[teamId]);\n"
    "                        }\n"
    "                    } else if (callback) {\n"
    "                        callback(*statePtr);\n"
    "                    }\n"
    "                });")
replace_once(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::retrieveCategories(BackendTeam& team,\n"
    "                                        std::function<void(const SidebarTeamState&)> callback)\n",
    "void SidebarService::retrieveCategories(BackendTeam& team,\n"
    "                                        std::function<void(const SidebarTeamState&)> callback,\n"
    "                                        bool storeResponse)\n")
replace_once(
    "sources/backend/SidebarService.cpp",
    "    retrieveChannelMemberships([this, teamId, callback] {\n"
    "        retrieveChannelPreferences([this, teamId, callback] {",
    "    retrieveChannelMemberships([this, teamId, callback, storeResponse] {\n"
    "        retrieveChannelPreferences([this, teamId, callback, storeResponse] {")
replace_once(
    "sources/backend/SidebarService.cpp",
    "                [this, teamId, callback](const QJsonDocument& doc) {",
    "                [this, teamId, callback, storeResponse](const QJsonDocument& doc) {")
replace_once(
    "sources/backend/SidebarService.cpp",
    "                    storeCategories(teamId, std::move(state), callback);",
    "                    storeCategories(teamId, std::move(state), callback, storeResponse);")

# Fix the temporary-container iterator bug and a Qt6 signed-size comparison.
replace_once(
    "sources/channel-tree/ChannelTreeReconcile.cpp",
    "        const QSet<QString> desired(desiredChannels.value(it.key()).cbegin(),\n"
    "                                    desiredChannels.value(it.key()).cend());",
    "        QSet<QString> desired;\n"
    "        const QStringList desiredIds = desiredChannels.value(it.key());\n"
    "        for (const QString& id : desiredIds) {\n"
    "            desired.insert(id);\n"
    "        }")
replace_once(
    "sources/channel-tree/ChannelTreeReconcile.cpp",
    "    while (teamItem.childCount() > active.size()) {\n"
    "        QTreeWidgetItem* staleCategory = teamItem.takeChild(active.size());",
    "    const int activeCategoryCount = static_cast<int>(active.size());\n"
    "    while (teamItem.childCount() > activeCategoryCount) {\n"
    "        QTreeWidgetItem* staleCategory = teamItem.takeChild(activeCategoryCount);")

# Add generation-checked server verification. retrieveCategories(..., false)
# leaves the optimistic service state untouched until the response is known to
# belong to the latest local mutation.
insert_anchor = "void ChannelTree::destroySidebarRow(QTreeWidgetItem* item)\n"
insert_text = r'''void ChannelTree::verifySidebarTeam(const QString& teamId, quint64 mutation)
{
    if (!backendForSidebar
        || sidebarMutationGeneration.value(teamId) != mutation) {
        return;
    }

    BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId);
    if (!team) {
        return;
    }

    QPointer<ChannelTree> guard(this);
    SidebarService::instance(*backendForSidebar).retrieveCategories(
        *team,
        [guard, teamId, mutation](const SidebarTeamState& serverState) {
            if (!guard || !guard->backendForSidebar
                || guard->sidebarMutationGeneration.value(teamId) != mutation) {
                return;
            }
            auto& sidebar = SidebarService::instance(*guard->backendForSidebar);
            sidebar.applyLocalTeamState(teamId, serverState);
            if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
                guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, serverState);
            }
        },
        false);
}

'''
replace_once("sources/channel-tree/ChannelTreeReconcile.cpp", insert_anchor,
             insert_text + insert_anchor)

# Successful writes and error rollback both verify through the non-mutating GET.
text = read("sources/channel-tree/ChannelTree.cpp")
text = text.replace("        guard->refreshSidebarTeam(teamId);\n    };",
                    "        guard->verifySidebarTeam(teamId, mutation);\n    };", 1)
text = text.replace("        guard->refreshSidebarTeam(teamId);\n    };",
                    "        guard->verifySidebarTeam(teamId, mutation);\n    };", 1)
text = text.replace("                guard->refreshSidebarTeam(teamId);",
                    "                guard->verifySidebarTeam(teamId, mutation);", 1)
text = text.replace("        guard->refreshSidebarTeam(teamId);\n    };",
                    "        guard->verifySidebarTeam(teamId, mutation);\n    };", 1)
write("sources/channel-tree/ChannelTree.cpp", text)

# Rewrite gap animation bookkeeping so interrupted animations always close every
# previously touched row rather than leaking stale size-hint roles.
path = "sources/channel-tree/ChannelTreeDragVisuals.cpp"
text = read(path)
text = text.replace("index.data(SidebarItem::DragCollapseRole).toReal()",
                    "index.data(SidebarItem::DragCollapseRole).toDouble()")
text = text.replace("    if (!dragSourceIndexes.isEmpty() || dragGapIndex.isValid()) {",
                    "    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {")
start = text.index("void ChannelTree::animateDropGap(")
end = text.index("\nvoid ChannelTree::updateDragVisuals", start)
new_func = r'''void ChannelTree::animateDropGap(const QPersistentModelIndex& target, bool after, int extent)
{
    stopAnimation(dropGapAnimation);

    QVector<QPersistentModelIndex> indexes;
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid() && !indexes.contains(index)) {
            indexes.push_back(index);
        }
    }
    if (target.isValid() && !indexes.contains(target)) {
        indexes.push_back(target);
    }
    dragGapIndexes = indexes;
    currentDragGapIndex = target;

    if (indexes.isEmpty()) {
        return;
    }

    QVector<int> startBefore;
    QVector<int> startAfter;
    startBefore.reserve(indexes.size());
    startAfter.reserve(indexes.size());
    for (const QPersistentModelIndex& index : indexes) {
        startBefore.push_back(gapValue(index, SidebarItem::DropGapBeforeRole));
        startAfter.push_back(gapValue(index, SidebarItem::DropGapAfterRole));
    }

    auto* animation = new QVariantAnimation(this);
    dropGapAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    const int wantedExtent = qMax(0, extent);
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, indexes, startBefore, startAfter,
             target, after, wantedExtent](const QVariant& value) {
        if (dropGapAnimation != animation) {
            return;
        }
        const qreal progress = value.toDouble();
        for (qsizetype i = 0; i < indexes.size(); ++i) {
            if (!indexes[i].isValid()) {
                continue;
            }
            const bool isTarget = indexes[i] == target;
            const int wantedBefore = isTarget && !after ? wantedExtent : 0;
            const int wantedAfter = isTarget && after ? wantedExtent : 0;
            model()->setData(indexes[i],
                             qRound(startBefore[i]
                                    + (wantedBefore - startBefore[i]) * progress),
                             SidebarItem::DropGapBeforeRole);
            model()->setData(indexes[i],
                             qRound(startAfter[i]
                                    + (wantedAfter - startAfter[i]) * progress),
                             SidebarItem::DropGapAfterRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, indexes, target] {
        if (dropGapAnimation != animation) {
            return;
        }
        for (const QPersistentModelIndex& index : indexes) {
            if (index.isValid() && index != target) {
                model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
                model()->setData(index, 0, SidebarItem::DropGapAfterRole);
            }
        }
        dropGapAnimation = nullptr;
        animation->deleteLater();
        dragGapIndexes.clear();
        if (target.isValid()) {
            dragGapIndexes.push_back(target);
        } else {
            currentDragGapIndex = QPersistentModelIndex();
        }
    });
    animation->start();
}
'''
text = text[:start] + new_func.rstrip() + text[end:]

old_clear = r'''void ChannelTree::clearDropGap(bool animate)
{
    if (!dragGapIndex.isValid()) {
        return;
    }
    if (animate) {
        animateDropGap(QPersistentModelIndex(), false, 0);
        return;
    }

    stopAnimation(dropGapAnimation);
    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    dragGapIndex = QPersistentModelIndex();
}'''
new_clear = r'''void ChannelTree::clearDropGap(bool animate)
{
    if (dragGapIndexes.isEmpty()) {
        return;
    }
    if (animate) {
        animateDropGap(QPersistentModelIndex(), false, 0);
        return;
    }

    stopAnimation(dropGapAnimation);
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(index, 0, SidebarItem::DropGapAfterRole);
        }
    }
    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
}'''
if old_clear not in text:
    raise RuntimeError("clearDropGap anchor missing")
text = text.replace(old_clear, new_clear, 1)

old_reset = r'''    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    for (const QPersistentModelIndex& index : dragSourceIndexes) {'''
new_reset = r'''    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(index, 0, SidebarItem::DropGapAfterRole);
        }
    }
    for (const QPersistentModelIndex& index : dragSourceIndexes) {'''
if old_reset not in text:
    raise RuntimeError("reset gap anchor missing")
text = text.replace(old_reset, new_reset, 1)
text = text.replace("    dragGapIndex = QPersistentModelIndex();\n    dragSourceIndexes.clear();",
                    "    dragGapIndexes.clear();\n"
                    "    currentDragGapIndex = QPersistentModelIndex();\n"
                    "    dragSourceIndexes.clear();", 1)
text = text.replace("        for (int i = 0; i < indexes.size(); ++i) {",
                    "        for (qsizetype i = 0; i < indexes.size(); ++i) {")
write(path, text)

# Fade the category disclosure arrow together with a collapsing category row.
replace_once(
    "sources/channel-tree/ChannelTreeBranches.cpp",
    "    style()->drawPrimitive(isExpanded(index)\n"
    "                               ? QStyle::PE_IndicatorArrowDown\n"
    "                               : QStyle::PE_IndicatorArrowRight,\n"
    "                           &option, painter, this);",
    "    const qreal collapse = qBound<qreal>(\n"
    "        0.0, index.data(SidebarItem::DragCollapseRole).toDouble(), 1.0);\n"
    "    painter->save();\n"
    "    painter->setOpacity(painter->opacity() * (1.0 - collapse));\n"
    "    style()->drawPrimitive(isExpanded(index)\n"
    "                               ? QStyle::PE_IndicatorArrowDown\n"
    "                               : QStyle::PE_IndicatorArrowRight,\n"
    "                           &option, painter, this);\n"
    "    painter->restore();")

print("sidebar DnD UX hardening applied")
