from pathlib import Path


def read(path):
    return Path(path).read_text()


def write(path, text):
    Path(path).write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing block in {path}: {old[:180]!r}")
    write(path, text.replace(old, new, 1))


# Keep the sidebar's canonical unread role synchronized with SidebarService.
replace_once(
    "sources/channel-tree/ChannelItem.h",
    "    void setMuted (bool muted);\n    void setMentioned (bool mentioned);\n",
    "    void setMuted (bool muted);\n    void setUnread (bool unread);\n    void setMentioned (bool mentioned);\n")

replace_once(
    "sources/channel-tree/ChannelItem.cpp",
    "void ChannelItem::setMentioned(bool mentioned)\n",
    "void ChannelItem::setUnread(bool unread)\n"
    "{\n"
    "    setData(0, SidebarItem::UnreadRole, unread);\n"
    "}\n\n"
    "void ChannelItem::setMentioned(bool mentioned)\n")

replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid setChannelMutedVisual(const QString& channelId, bool muted);\n"
    "\tvoid setChannelMentionedVisual(const QString& channelId, bool mentioned);\n",
    "\tvoid setChannelMutedVisual(const QString& channelId, bool muted);\n"
    "\tvoid setChannelUnreadVisual(const QString& channelId, bool unread);\n"
    "\tvoid setChannelMentionedVisual(const QString& channelId, bool mentioned);\n")

replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "    connect(&sidebar, &SidebarService::channelMutedChanged,\n"
    "            this, &ChannelTree::setChannelMutedVisual, Qt::UniqueConnection);\n"
    "    connect(&sidebar, &SidebarService::channelMentionedChanged,\n",
    "    connect(&sidebar, &SidebarService::channelMutedChanged,\n"
    "            this, &ChannelTree::setChannelMutedVisual, Qt::UniqueConnection);\n"
    "    connect(&sidebar, &SidebarService::channelActivityChanged, this,\n"
    "            [this, &sidebar](const QString& channelId) {\n"
    "        if (!backendForSidebar) {\n"
    "            return;\n"
    "        }\n"
    "        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);\n"
    "        if (channel) {\n"
    "            setChannelUnreadVisual(channelId, sidebar.isChannelUnread(*channel));\n"
    "        }\n"
    "    }, Qt::UniqueConnection);\n"
    "    connect(&sidebar, &SidebarService::channelMentionedChanged,\n")

replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "    item->setMuted(sidebar.isChannelMuted(channel));\n"
    "    item->setMentioned(sidebar.hasUnreadMention(channel.id));\n",
    "    item->setMuted(sidebar.isChannelMuted(channel));\n"
    "    item->setUnread(sidebar.isChannelUnread(channel));\n"
    "    item->setMentioned(sidebar.hasUnreadMention(channel.id));\n")

replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::setChannelMentionedVisual(const QString& channelId, bool mentioned)\n",
    "void ChannelTree::setChannelUnreadVisual(const QString& channelId, bool unread)\n"
    "{\n"
    "    const auto items = channelToItemMap.value(channelId);\n"
    "    for (QTreeWidgetItem* item : items) {\n"
    "        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {\n"
    "            static_cast<ChannelItem*>(item)->setUnread(unread);\n"
    "        }\n"
    "    }\n"
    "}\n\n"
    "void ChannelTree::setChannelMentionedVisual(const QString& channelId, bool mentioned)\n")

# A manual unread thread must survive a CRT snapshot that does not contain it
# yet (or at all if it is not followed). Also cancel an older optimistic read
# acknowledgement before installing the new unread boundary.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "    entry->readThroughPostId.clear();\n"
    "    entry->readThroughCreateAt = 0;\n"
    "    entry->lastReplyAt = std::max(entry->lastReplyAt, createAt);\n",
    "    entry->readThroughPostId.clear();\n"
    "    entry->readThroughCreateAt = 0;\n"
    "    entry->readAcknowledgementPending = false;\n"
    "    entry->readAcknowledgementAt = 0;\n"
    "    entry->lastReplyAt = std::max(entry->lastReplyAt, createAt);\n")

replace_once(
    "sources/backend/FollowingModel.cpp",
    "    // Keep only temporary root mentions that have not yet been replaced by a\n"
    "    // real server ThreadResponse. They are already part of the shared structure.\n",
    "    // A locally requested Mark as unread is authoritative for this client\n"
    "    // until the lower-edge read rule consumes it. A followed-thread snapshot\n"
    "    // may have started before set_unread, and a non-followed thread may not\n"
    "    // appear in that snapshot at all, so retain such entries explicitly.\n"
    "    for (const Entry& old : std::as_const(entries_)) {\n"
    "        if (!old.isThread()\n"
    "            || !manualAttentionKeys_.contains(manualUnreadKey(old.channelId, old.threadId))) {\n"
    "            continue;\n"
    "        }\n"
    "        bool present = false;\n"
    "        for (const Entry& candidate : std::as_const(next)) {\n"
    "            if (candidate.isThread() && candidate.threadId == old.threadId) {\n"
    "                present = true;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "        if (!present) {\n"
    "            next.push_back(old);\n"
    "        }\n"
    "    }\n\n"
    "    // Keep only temporary root mentions that have not yet been replaced by a\n"
    "    // real server ThreadResponse. They are already part of the shared structure.\n")

# If a marked post disappears (delete/remap) there is no edge left to re-enter;
# do not freeze read progression forever.
replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    if (manualUnreadGate_.active()) {\n"
    "        const QString gatedPostId = manualUnreadGate_.postId();\n"
    "        const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);\n",
    "    if (manualUnreadGate_.active()) {\n"
    "        const QString gatedPostId = manualUnreadGate_.postId();\n"
    "        if (postSource->indexOfPost(gatedPostId) < 0) {\n"
    "            manualUnreadGate_.clear();\n"
    "        }\n"
    "        const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);\n")

# Avoid testing a cleared gate once the target disappeared.
replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "        const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);\n"
    "        if (manualUnreadGate_.update(lowerEdgeVisible)) {\n",
    "        const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);\n"
    "        if (manualUnreadGate_.active() && manualUnreadGate_.update(lowerEdgeVisible)) {\n")
