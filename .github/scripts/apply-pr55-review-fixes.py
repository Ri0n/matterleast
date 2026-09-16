from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:160]!r}")
    p.write_text(text.replace(old, new, 1))


def insert_before_once(path, marker, addition):
    p = Path(path)
    text = p.read_text()
    if marker not in text:
        raise SystemExit(f"marker not found in {path}: {marker[:160]!r}")
    p.write_text(text.replace(marker, addition + marker, 1))


# ChannelActivityTracker: remember the selected unread domain per channel and
# retain root/reply runtime activity separately so changing/falling back domains
# cannot misclassify an already received reply.
replace_once(
    "sources/backend/ChannelActivityTracker.h",
    "        bool hasReadRootMessageCount = false;\n"
    "        bool hasRootMentionCount = false;\n\n"
    "        bool serverUnreadActivity = false;\n"
    "        bool runtimeUnreadActivity = false;\n"
    "        bool serverMentioned = false;\n"
    "        bool runtimeMentioned = false;\n",
    "        bool hasReadRootMessageCount = false;\n"
    "        bool hasRootMentionCount = false;\n"
    "        bool rootUnreadMode = false;\n\n"
    "        bool serverUnreadActivity = false;\n"
    "        bool runtimeUnreadActivity = false;\n"
    "        bool runtimeReplyUnreadActivity = false;\n"
    "        bool serverMentioned = false;\n"
    "        bool runtimeMentioned = false;\n"
    "        bool runtimeReplyMentioned = false;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.h",
    "    bool usesRootUnreadCounts(const QString& channelId, bool hasTotalRootMessageCount) const;\n",
    "    bool usesRootUnreadCounts(const QString& channelId) const;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.h",
    "private:\n    QHash<QString, Entry> entries;\n    bool collapsedThreadsEnabled = false;\n",
    "private:\n    QHash<QString, Entry> entries;\n")

replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "void ChannelActivityTracker::clear()\n"
    "{\n"
    "    entries.clear();\n"
    "    collapsedThreadsEnabled = false;\n"
    "}\n",
    "void ChannelActivityTracker::clear()\n"
    "{\n"
    "    entries.clear();\n"
    "}\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "{\n"
    "    collapsedThreadsEnabled = useCollapsedThreads;\n\n"
    "    auto it = entries.find(channelId);\n",
    "{\n"
    "    auto it = entries.find(channelId);\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    const bool useRootCounts = usesRootUnreadCounts(channelId, hasTotalRootMessageCount);\n"
    "    const uint64_t readCount = useRootCounts ? entry.readRootMessageCount : entry.readMessageCount;\n"
    "    const uint64_t totalCount = useRootCounts ? totalRootMessageCount : totalMessageCount;\n\n"
    "    entry.serverUnreadActivity = totalCount > readCount;\n"
    "    entry.serverMentioned = collapsedThreadsEnabled && entry.hasRootMentionCount\n"
    "        ? entry.rootMentionCount > 0\n"
    "        : entry.mentionCount > 0;\n",
    "    entry.rootUnreadMode = useCollapsedThreads\n"
    "        && entry.hasReadRootMessageCount\n"
    "        && hasTotalRootMessageCount;\n"
    "    const uint64_t readCount = entry.rootUnreadMode\n"
    "        ? entry.readRootMessageCount : entry.readMessageCount;\n"
    "    const uint64_t totalCount = entry.rootUnreadMode\n"
    "        ? totalRootMessageCount : totalMessageCount;\n\n"
    "    entry.serverUnreadActivity = totalCount > readCount;\n"
    "    entry.serverMentioned = entry.rootUnreadMode && entry.hasRootMentionCount\n"
    "        ? entry.rootMentionCount > 0\n"
    "        : entry.mentionCount > 0;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    // With CRT enabled, reply activity and mentions belong to the followed\n"
    "    // thread model, not to the parent channel's root counters. With CRT off,\n"
    "    // replies are ordinary channel activity and use the normal counters.\n"
    "    const bool belongsToParentChannel = !threadReply || !collapsedThreadsEnabled;\n"
    "    if (mentioned && belongsToParentChannel) {\n"
    "        entry.runtimeMentioned = true;\n"
    "    }\n\n"
    "    if (ownPost) {\n"
    "        return;\n"
    "    }\n\n"
    "    if (belongsToParentChannel) {\n"
    "        entry.runtimeUnreadActivity = true;\n"
    "    }\n",
    "    // Preserve both runtime domains. Whether reply activity belongs to the\n"
    "    // parent is decided by the same root-counter mode as server unread\n"
    "    // state, so a late membership/channel snapshot can safely change the\n"
    "    // selected domain without losing or inventing WS activity.\n"
    "    if (mentioned) {\n"
    "        if (threadReply) {\n"
    "            entry.runtimeReplyMentioned = true;\n"
    "        } else {\n"
    "            entry.runtimeMentioned = true;\n"
    "        }\n"
    "    }\n\n"
    "    if (ownPost) {\n"
    "        return;\n"
    "    }\n\n"
    "    if (threadReply) {\n"
    "        entry.runtimeReplyUnreadActivity = true;\n"
    "    } else {\n"
    "        entry.runtimeUnreadActivity = true;\n"
    "    }\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    entry.serverUnreadActivity = false;\n"
    "    entry.runtimeUnreadActivity = false;\n"
    "    entry.serverMentioned = false;\n"
    "    entry.runtimeMentioned = false;\n",
    "    entry.serverUnreadActivity = false;\n"
    "    entry.runtimeUnreadActivity = false;\n"
    "    entry.runtimeReplyUnreadActivity = false;\n"
    "    entry.serverMentioned = false;\n"
    "    entry.runtimeMentioned = false;\n"
    "    entry.runtimeReplyMentioned = false;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    entry.serverUnreadActivity = true;\n"
    "    entry.runtimeUnreadActivity = false;\n"
    "    entry.serverMentioned = collapsedThreadsEnabled && hasRootMentionCount\n"
    "        ? rootMentionCount > 0\n"
    "        : mentionCount > 0;\n"
    "    entry.runtimeMentioned = false;\n",
    "    entry.serverUnreadActivity = true;\n"
    "    entry.runtimeUnreadActivity = false;\n"
    "    entry.runtimeReplyUnreadActivity = false;\n"
    "    entry.serverMentioned = entry.rootUnreadMode && hasRootMentionCount\n"
    "        ? rootMentionCount > 0\n"
    "        : mentionCount > 0;\n"
    "    entry.runtimeMentioned = false;\n"
    "    entry.runtimeReplyMentioned = false;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "        it->serverMentioned = false;\n"
    "        it->runtimeMentioned = false;\n",
    "        it->serverMentioned = false;\n"
    "        it->runtimeMentioned = false;\n"
    "        it->runtimeReplyMentioned = false;\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    const Entry& entry = it.value();\n"
    "    const bool mentioned = entry.serverMentioned || entry.runtimeMentioned;\n"
    "    const bool unreadActivity = entry.serverUnreadActivity || entry.runtimeUnreadActivity;\n"
    "    return mentioned || (!entry.muted && unreadActivity);\n",
    "    const Entry& entry = it.value();\n"
    "    const bool mentioned = entry.serverMentioned || entry.runtimeMentioned\n"
    "        || (!entry.rootUnreadMode && entry.runtimeReplyMentioned);\n"
    "    const bool unreadActivity = entry.serverUnreadActivity || entry.runtimeUnreadActivity\n"
    "        || (!entry.rootUnreadMode && entry.runtimeReplyUnreadActivity);\n"
    "    return mentioned || (!entry.muted && unreadActivity);\n")
replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    const auto it = entries.constFind(channelId);\n"
    "    return it != entries.cend() && (it->serverMentioned || it->runtimeMentioned);\n"
    "}\n\n"
    "bool ChannelActivityTracker::usesRootUnreadCounts(const QString& channelId,\n"
    "                                                   bool hasTotalRootMessageCount) const\n"
    "{\n"
    "    const auto it = entries.constFind(channelId);\n"
    "    return collapsedThreadsEnabled\n"
    "        && hasTotalRootMessageCount\n"
    "        && it != entries.cend()\n"
    "        && it->tracked\n"
    "        && it->hasReadRootMessageCount;\n"
    "}\n",
    "    const auto it = entries.constFind(channelId);\n"
    "    if (it == entries.cend()) {\n"
    "        return false;\n"
    "    }\n"
    "    return it->serverMentioned || it->runtimeMentioned\n"
    "        || (!it->rootUnreadMode && it->runtimeReplyMentioned);\n"
    "}\n\n"
    "bool ChannelActivityTracker::usesRootUnreadCounts(const QString& channelId) const\n"
    "{\n"
    "    const auto it = entries.constFind(channelId);\n"
    "    return it != entries.cend() && it->tracked && it->rootUnreadMode;\n"
    "}\n")

replace_once(
    "sources/backend/SidebarService.cpp",
    "    return activityTracker.usesRootUnreadCounts(channel.id, channel.has_total_msg_count_root);\n",
    "    return activityTracker.usesRootUnreadCounts(channel.id);\n")

# FollowingModel: make the conversation read domain explicit at the read-through
# boundary. This removes the hidden SidebarService lookup from next-post search
# and lets the integration test drive the exact same root-only transition.
replace_once(
    "sources/backend/FollowingModel.h",
    "    void observeReadThrough(const QString& channelId,\n"
    "                            const QString& threadId,\n"
    "                            const BackendPost& post,\n"
    "                            bool sourceAtEnd);\n",
    "    void observeReadThrough(const QString& channelId,\n"
    "                            const QString& threadId,\n"
    "                            const BackendPost& post,\n"
    "                            bool sourceAtEnd,\n"
    "                            bool rootOnlyConversation = false);\n")
replace_once(
    "sources/backend/FollowingModel.h",
    "    QString nextCachedPostId(const BackendChannel& channel,\n"
    "                             const QString& threadId,\n"
    "                             const BackendPost& after) const;\n",
    "    QString nextCachedPostId(const BackendChannel& channel,\n"
    "                             const QString& threadId,\n"
    "                             const BackendPost& after,\n"
    "                             bool rootOnlyConversation) const;\n")
replace_once(
    "sources/backend/FollowingModel.cpp",
    "QString FollowingModel::nextCachedPostId(const BackendChannel& channel,\n"
    "                                         const QString& threadId,\n"
    "                                         const BackendPost& after) const\n"
    "{\n"
    "    const bool rootOnlyConversation = threadId.isEmpty()\n"
    "        && SidebarService::instance(backend_).usesRootUnreadCounts(channel);\n",
    "QString FollowingModel::nextCachedPostId(const BackendChannel& channel,\n"
    "                                         const QString& threadId,\n"
    "                                         const BackendPost& after,\n"
    "                                         bool rootOnlyConversation) const\n"
    "{\n"
    "    rootOnlyConversation = rootOnlyConversation && threadId.isEmpty();\n")
replace_once(
    "sources/backend/FollowingModel.cpp",
    "void FollowingModel::observeReadThrough(const QString& channelId,\n"
    "                                        const QString& threadId,\n"
    "                                        const BackendPost& post,\n"
    "                                        bool sourceAtEnd)\n",
    "void FollowingModel::observeReadThrough(const QString& channelId,\n"
    "                                        const QString& threadId,\n"
    "                                        const BackendPost& post,\n"
    "                                        bool sourceAtEnd,\n"
    "                                        bool rootOnlyConversation)\n")
replace_once(
    "sources/backend/FollowingModel.cpp",
    "    const QString nextPostId = nextCachedPostId(*channel, threadId, post);\n",
    "    const QString nextPostId = nextCachedPostId(\n"
    "        *channel, threadId, post, rootOnlyConversation);\n")

replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    followingModel.observeReadThrough(channel.id, QString(), *readPost, channelAtEnd);\n",
    "    followingModel.observeReadThrough(\n"
    "        channel.id, QString(), *readPost, channelAtEnd, rootUnreadConversation);\n")

# Tracker coverage: prove the selected domain is per-entry and that a reply
# received while CRT is root-only reappears as parent activity if root counters
# become unavailable (and disappears again when root mode is restored).
replace_once(
    "tests/ChannelActivityTrackerTest.cpp",
    "    void crtRootUnreadModeRequiresBothRootCounters()\n"
    "    {\n"
    "        ChannelActivityTracker tracker;\n"
    "        setMembership(tracker, 1000, 20, 5, true);\n"
    "        synchronize(tracker, 2000, 20, 5, true, true);\n"
    "        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral(\"channel\"), true));\n\n"
    "        QVERIFY(!tracker.usesRootUnreadCounts(QStringLiteral(\"channel\"), false));\n\n"
    "        ChannelActivityTracker noMembershipRootCount;\n"
    "        setMembership(noMembershipRootCount, 1000, 20, 0, false);\n"
    "        synchronize(noMembershipRootCount, 2000, 20, 5, true, true);\n"
    "        QVERIFY(!noMembershipRootCount.usesRootUnreadCounts(\n"
    "            QStringLiteral(\"channel\"), true));\n"
    "    }\n",
    "    void crtRootUnreadModeRequiresBothRootCounters()\n"
    "    {\n"
    "        ChannelActivityTracker tracker;\n"
    "        setMembership(tracker, 1000, 20, 5, true);\n"
    "        synchronize(tracker, 2000, 20, 5, true, true);\n"
    "        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n\n"
    "        ChannelActivityTracker noChannelRootCount;\n"
    "        setMembership(noChannelRootCount, 1000, 20, 5, true);\n"
    "        synchronize(noChannelRootCount, 2000, 20, 0, false, true);\n"
    "        QVERIFY(!noChannelRootCount.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n\n"
    "        ChannelActivityTracker noMembershipRootCount;\n"
    "        setMembership(noMembershipRootCount, 1000, 20, 0, false);\n"
    "        synchronize(noMembershipRootCount, 2000, 20, 5, true, true);\n"
    "        QVERIFY(!noMembershipRootCount.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n"
    "    }\n\n"
    "    void crtFallbackReclassifiesReplyRuntimeActivity()\n"
    "    {\n"
    "        ChannelActivityTracker tracker;\n"
    "        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, false);\n"
    "        synchronize(tracker, 1000, 5, 5, true, true);\n"
    "        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n\n"
    "        tracker.recordPost(QStringLiteral(\"channel\"), 2000, false, true, true);\n"
    "        QVERIFY(!tracker.isUnread(QStringLiteral(\"channel\")));\n"
    "        QVERIFY(!tracker.hasMention(QStringLiteral(\"channel\")));\n\n"
    "        // If the channel-side root counter disappears, the same already\n"
    "        // received reply belongs to the whole-message fallback domain.\n"
    "        synchronize(tracker, 2000, 5, 0, false, true);\n"
    "        QVERIFY(!tracker.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n"
    "        QVERIFY(tracker.isUnread(QStringLiteral(\"channel\")));\n"
    "        QVERIFY(tracker.hasMention(QStringLiteral(\"channel\")));\n\n"
    "        // Restoring authoritative root counters moves the reply back to\n"
    "        // thread-only activity without losing its runtime bookkeeping.\n"
    "        synchronize(tracker, 2000, 5, 5, true, true);\n"
    "        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral(\"channel\")));\n"
    "        QVERIFY(!tracker.isUnread(QStringLiteral(\"channel\")));\n"
    "        QVERIFY(!tracker.hasMention(QStringLiteral(\"channel\")));\n"
    "    }\n")

# Integration regression: actual Following/Attention state for both direct and
# group conversations. A root-tail read must retire only the parent conversation
# even when a newer hidden reply remains unread in its thread.
insert_before_once(
    "tests/ManualUnreadReadStateTest.cpp",
    "} // namespace\n",
    "BackendChannel* makeConversationChannel(Backend& backend, const QString& type)\n"
    "{\n"
    "    Storage& storage = backend.getStorage();\n"
    "    storage.addUser(QJsonObject {\n"
    "        {QStringLiteral(\"id\"), QStringLiteral(\"me\")},\n"
    "        {QStringLiteral(\"username\"), QStringLiteral(\"me\")},\n"
    "    }, true);\n"
    "    storage.addUser(QJsonObject {\n"
    "        {QStringLiteral(\"id\"), QStringLiteral(\"user\")},\n"
    "        {QStringLiteral(\"username\"), QStringLiteral(\"user\")},\n"
    "    });\n\n"
    "    QJsonObject json {\n"
    "        {QStringLiteral(\"id\"), QStringLiteral(\"channel\")},\n"
    "        {QStringLiteral(\"type\"), type},\n"
    "        {QStringLiteral(\"name\"), type == QStringLiteral(\"D\")\n"
    "            ? QStringLiteral(\"me__user\") : QStringLiteral(\"group\")},\n"
    "        {QStringLiteral(\"display_name\"), QStringLiteral(\"me, user\")},\n"
    "        {QStringLiteral(\"last_post_at\"), 200.0},\n"
    "        {QStringLiteral(\"last_root_post_at\"), 100.0},\n"
    "        {QStringLiteral(\"total_msg_count\"), 2},\n"
    "        {QStringLiteral(\"total_msg_count_root\"), 1},\n"
    "    };\n\n"
    "    return type == QStringLiteral(\"D\")\n"
    "        ? storage.addDirectChannel(json)\n"
    "        : storage.addGroupChannel(json);\n"
    "}\n\n")
insert_before_once(
    "tests/ManualUnreadReadStateTest.cpp",
    "};\n\nQTEST_MAIN(ManualUnreadReadStateTest)\n",
    "    void dmGmRootReadLeavesHiddenThreadUnread_data()\n"
    "    {\n"
    "        QTest::addColumn<QString>(\"channelType\");\n"
    "        QTest::newRow(\"direct\") << QStringLiteral(\"D\");\n"
    "        QTest::newRow(\"group\") << QStringLiteral(\"G\");\n"
    "    }\n\n"
    "    void dmGmRootReadLeavesHiddenThreadUnread()\n"
    "    {\n"
    "        QFETCH(QString, channelType);\n\n"
    "        Backend backend;\n"
    "        BackendChannel* channel = makeConversationChannel(backend, channelType);\n"
    "        QVERIFY(channel);\n"
    "        QCOMPARE(channel->last_root_post_at, uint64_t(100));\n"
    "        QCOMPARE(channel->last_post_at, uint64_t(200));\n\n"
    "        BackendPost* root = channel->addPost(postJson(QStringLiteral(\"root\"), 100));\n"
    "        BackendPost* reply = channel->addPost(\n"
    "            postJson(QStringLiteral(\"reply\"), 200, root->id));\n"
    "        QVERIFY(root);\n"
    "        QVERIFY(reply);\n\n"
    "        FollowingModel& model = FollowingModel::instance(backend);\n"
    "        model.markPostUnread(channel->id, QString(), root->id, root->create_at);\n"
    "        model.markPostUnread(channel->id, root->id, reply->id, reply->create_at);\n\n"
    "        const FollowingModel::Entry* parentBefore = model.findEntry(channel->id);\n"
    "        const FollowingModel::Entry* threadBefore = model.findEntry(channel->id, root->id);\n"
    "        QVERIFY(parentBefore);\n"
    "        QVERIFY(parentBefore->requiresAttention());\n"
    "        QVERIFY(threadBefore);\n"
    "        QVERIFY(threadBefore->requiresAttention());\n\n"
    "        // This is the DM/GM regression: the root source is fully consumed\n"
    "        // at last_root_post_at, while a newer reply exists only in the\n"
    "        // collapsed thread. Parent Attention must disappear, but the\n"
    "        // thread's unread state must remain intact.\n"
    "        model.observeReadThrough(channel->id, QString(), *root, true, true);\n\n"
    "        QVERIFY(!model.findEntry(channel->id));\n"
    "        const FollowingModel::Entry* threadAfter =\n"
    "            model.findEntry(channel->id, root->id);\n"
    "        QVERIFY(threadAfter);\n"
    "        QVERIFY(threadAfter->requiresAttention());\n"
    "        QCOMPARE(threadAfter->resumeState, FollowingModel::ResumeState::FirstUnread);\n"
    "        QCOMPARE(threadAfter->firstUnreadPostId, reply->id);\n"
    "    }\n\n")
