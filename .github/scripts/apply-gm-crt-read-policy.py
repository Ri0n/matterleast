from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:120]!r}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "sources/backend/ChannelActivityTracker.h",
    "    bool hasMention(const QString& channelId) const;\n"
    "    uint64_t activityTime(const QString& channelId) const;\n",
    "    bool hasMention(const QString& channelId) const;\n"
    "    /** Whether CRT root counters, rather than all-message counters, define parent unread state. */\n"
    "    bool usesRootUnreadCounts(const QString& channelId, bool hasTotalRootMessageCount) const;\n"
    "    uint64_t activityTime(const QString& channelId) const;\n")

replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "    const bool useRootCounts = collapsedThreadsEnabled\n"
    "        && entry.hasReadRootMessageCount\n"
    "        && hasTotalRootMessageCount;\n",
    "    const bool useRootCounts = usesRootUnreadCounts(channelId, hasTotalRootMessageCount);\n")

replace_once(
    "sources/backend/ChannelActivityTracker.cpp",
    "bool ChannelActivityTracker::hasMention(const QString& channelId) const\n"
    "{\n"
    "    const auto it = entries.constFind(channelId);\n"
    "    return it != entries.cend() && (it->serverMentioned || it->runtimeMentioned);\n"
    "}\n\n"
    "uint64_t ChannelActivityTracker::activityTime(const QString& channelId) const\n",
    "bool ChannelActivityTracker::hasMention(const QString& channelId) const\n"
    "{\n"
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
    "}\n\n"
    "uint64_t ChannelActivityTracker::activityTime(const QString& channelId) const\n")

replace_once(
    "sources/backend/SidebarService.h",
    "    bool isChannelTracked(const QString& channelId) const;\n"
    "    bool isChannelUnread(const BackendChannel& channel) const;\n"
    "    uint64_t channelActivityTime(const BackendChannel& channel) const;\n",
    "    bool isChannelTracked(const QString& channelId) const;\n"
    "    bool isChannelUnread(const BackendChannel& channel) const;\n"
    "    /** True when parent-channel unread state intentionally excludes CRT replies. */\n"
    "    bool usesRootUnreadCounts(const BackendChannel& channel) const;\n"
    "    uint64_t channelActivityTime(const BackendChannel& channel) const;\n")

replace_once(
    "sources/backend/SidebarService.cpp",
    "bool SidebarService::isChannelUnread(const BackendChannel& channel) const\n"
    "{\n"
    "    return activityTracker.isUnread(channel.id);\n"
    "}\n\n"
    "uint64_t SidebarService::channelActivityTime(const BackendChannel& channel) const\n",
    "bool SidebarService::isChannelUnread(const BackendChannel& channel) const\n"
    "{\n"
    "    return activityTracker.isUnread(channel.id);\n"
    "}\n\n"
    "bool SidebarService::usesRootUnreadCounts(const BackendChannel& channel) const\n"
    "{\n"
    "    return activityTracker.usesRootUnreadCounts(channel.id, channel.has_total_msg_count_root);\n"
    "}\n\n"
    "uint64_t SidebarService::channelActivityTime(const BackendChannel& channel) const\n")

replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    BackendChannel& channel = chatArea->getChannel();\n"
    "    auto& followingModel = FollowingModel::instance(*backend);\n"
    "    const bool sourceTailRead = readIndex == postSource->itemCount() - 1;\n",
    "    BackendChannel& channel = chatArea->getChannel();\n"
    "    auto& followingModel = FollowingModel::instance(*backend);\n"
    "    auto& sidebar = SidebarService::instance(*backend);\n"
    "    const bool sourceTailRead = readIndex == postSource->itemCount() - 1;\n"
    "    const bool rootUnreadConversation = sidebar.usesRootUnreadCounts(channel);\n")

replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "        // A DM/GM Following row represents the whole conversation, including\n"
    "        // replies hidden behind collapsed threads. Only the actual latest\n"
    "        // channel activity may consume that conversation-level unread state.\n"
    "        if (channel.type == BackendChannel::directChannel\n"
    "            || channel.type == BackendChannel::groupChannel) {\n"
    "            const bool channelAtEnd = threadAtEnd\n"
    "                && (channel.last_post_at == 0\n"
    "                    || readPost->create_at >= channel.last_post_at);\n"
    "            followingModel.observeReadThrough(channel.id, QString(),\n"
    "                                              *readPost, channelAtEnd);\n"
    "            if (channelAtEnd) {\n"
    "                acknowledgeChannelRead(*backend, channel);\n"
    "            }\n"
    "        }\n",
    "        // With CRT root counters, replies are owned by the thread unread\n"
    "        // domain and must not advance or acknowledge the parent DM/GM. On\n"
    "        // servers/modes where replies still count for the channel, preserve\n"
    "        // the legacy whole-conversation behavior.\n"
    "        if ((channel.type == BackendChannel::directChannel\n"
    "             || channel.type == BackendChannel::groupChannel)\n"
    "            && !rootUnreadConversation) {\n"
    "            const bool channelAtEnd = threadAtEnd\n"
    "                && (channel.last_post_at == 0\n"
    "                    || readPost->create_at >= channel.last_post_at);\n"
    "            followingModel.observeReadThrough(channel.id, QString(),\n"
    "                                              *readPost, channelAtEnd);\n"
    "            if (channelAtEnd) {\n"
    "                acknowledgeChannelRead(*backend, channel);\n"
    "            }\n"
    "        }\n")

replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    // For ordinary channel timelines, the logical source tail is the visible\n"
    "    // channel end. DM/GM conversations additionally include collapsed replies,\n"
    "    // so do not clear their conversation unread state while newer activity is\n"
    "    // known to exist outside this root-post source.\n"
    "    bool channelAtEnd = sourceTailRead;\n"
    "    if (channel.type == BackendChannel::directChannel\n"
    "        || channel.type == BackendChannel::groupChannel) {\n"
    "        channelAtEnd = channelAtEnd\n"
    "            && (channel.last_post_at == 0\n"
    "                || readPost->create_at >= channel.last_post_at);\n"
    "    }\n",
    "    // The main ChannelPostSource contains roots only. When CRT root counters\n"
    "    // define parent unread state, compare its tail with last_root_post_at; a\n"
    "    // newer hidden reply belongs to the thread domain and must not keep a\n"
    "    // DM/GM permanently unread. With CRT off (or root counters unavailable),\n"
    "    // replies still belong to the whole conversation and last_post_at remains\n"
    "    // the correct acknowledgement watermark.\n"
    "    bool channelAtEnd = sourceTailRead;\n"
    "    if (channel.type == BackendChannel::directChannel\n"
    "        || channel.type == BackendChannel::groupChannel) {\n"
    "        const uint64_t conversationTail = rootUnreadConversation\n"
    "            ? channel.last_root_post_at : channel.last_post_at;\n"
    "        channelAtEnd = channelAtEnd\n"
    "            && (conversationTail == 0\n"
    "                || readPost->create_at >= conversationTail);\n"
    "    }\n")

replace_once(
    "sources/backend/FollowingModel.cpp",
    "    if (!post.isOwnPost()) {\n"
    "        if (Entry* conversation = findEntryMutable(channel.id, QString())) {\n"
    "            noteIncomingForResume(*conversation, post);\n"
    "        }\n"
    "        if (!post.root_id.isEmpty()) {\n",
    "    if (!post.isOwnPost()) {\n"
    "        const bool rootUnreadConversation =\n"
    "            SidebarService::instance(backend_).usesRootUnreadCounts(channel);\n"
    "        if ((!rootUnreadConversation || post.root_id.isEmpty())) {\n"
    "            if (Entry* conversation = findEntryMutable(channel.id, QString())) {\n"
    "                noteIncomingForResume(*conversation, post);\n"
    "            }\n"
    "        }\n"
    "        if (!post.root_id.isEmpty()) {\n")

replace_once(
    "sources/backend/FollowingModel.cpp",
    "QString FollowingModel::nextCachedPostId(const BackendChannel& channel,\n"
    "                                         const QString& threadId,\n"
    "                                         const BackendPost& after) const\n"
    "{\n"
    "    const BackendPost* next = nullptr;\n"
    "    for (const BackendPost& candidate : channel.posts) {\n"
    "        if (candidate.isDeleted || candidate.id.isEmpty()) {\n"
    "            continue;\n"
    "        }\n"
    "        if (!threadId.isEmpty()\n"
    "            && candidate.id != threadId\n"
    "            && candidate.root_id != threadId) {\n"
    "            continue;\n"
    "        }\n",
    "QString FollowingModel::nextCachedPostId(const BackendChannel& channel,\n"
    "                                         const QString& threadId,\n"
    "                                         const BackendPost& after) const\n"
    "{\n"
    "    const bool rootOnlyConversation = threadId.isEmpty()\n"
    "        && SidebarService::instance(backend_).usesRootUnreadCounts(channel);\n"
    "    const BackendPost* next = nullptr;\n"
    "    for (const BackendPost& candidate : channel.posts) {\n"
    "        if (candidate.isDeleted || candidate.id.isEmpty()) {\n"
    "            continue;\n"
    "        }\n"
    "        if (rootOnlyConversation && !candidate.root_id.isEmpty()) {\n"
    "            continue;\n"
    "        }\n"
    "        if (!threadId.isEmpty()\n"
    "            && candidate.id != threadId\n"
    "            && candidate.root_id != threadId) {\n"
    "            continue;\n"
    "        }\n")

replace_once(
    "tests/ChannelActivityTrackerTest.cpp",
    "    void crtUsesRootMentionCount()\n",
    "    void crtRootUnreadModeRequiresBothRootCounters()\n"
    "    {\n"
    "        ChannelActivityTracker tracker;\n"
    "        setMembership(tracker, 1000, 20, 5, true);\n"
    "        synchronize(tracker, 2000, 20, 5, true, true);\n"
    "        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral(\"channel\"), true));\n"
    "\n"
    "        QVERIFY(!tracker.usesRootUnreadCounts(QStringLiteral(\"channel\"), false));\n"
    "\n"
    "        ChannelActivityTracker noMembershipRootCount;\n"
    "        setMembership(noMembershipRootCount, 1000, 20, 0, false);\n"
    "        synchronize(noMembershipRootCount, 2000, 20, 5, true, true);\n"
    "        QVERIFY(!noMembershipRootCount.usesRootUnreadCounts(\n"
    "            QStringLiteral(\"channel\"), true));\n"
    "    }\n"
    "\n"
    "    void crtUsesRootMentionCount()\n")

old_doc = """Direct and group conversations need one extra guard. Their Following row
represents the whole conversation, including replies that may live in collapsed
threads and therefore are not part of the central root-post source. Reaching the
root-post tail must not consume a newer hidden reply. For DM/GM, conversation
acknowledgement therefore also requires that the read post reaches the channel's
latest activity (`last_post_at`). Reading the actual newest reply in its thread
advances both the thread cursor and the DM/GM conversation cursor.

```mermaid
flowchart TD
    A[Latest lower-edge-visible post] --> B{Thread view?}
    B -- no --> C{At root source tail?}
    C -- no --> D[Advance local cursor only]
    C -- yes --> E{DM/GM?}
    E -- no --> F[Mark channel viewed]
    E -- yes --> G{Read post reaches channel last_post_at?}
    G -- no --> D
    G -- yes --> F
    B -- yes --> H{Authoritative thread tail?}
    H -- no --> D
    H -- yes --> I[Mark thread read]
    I --> J{DM/GM and latest channel activity?}
    J -- yes --> F
    J -- no --> K[Thread acknowledgement only]
```
"""
new_doc = """Direct and group conversations follow the same unread domain selected by
`ChannelActivityTracker`. With Collapsed Reply Threads enabled *and* both
membership/channel root counters available, the parent conversation contains
root posts only for unread/resume purposes. A hidden reply belongs to its thread
entry and must not keep the parent DM/GM unread. The main root source therefore
uses `last_root_post_at` as its acknowledgement watermark; reading a thread
advances/acknowledges only that thread.

When CRT is disabled, or root counters are unavailable, replies still belong to
the parent channel unread domain. In that compatibility mode DM/GM keeps the
whole-conversation rule: the acknowledgement watermark is `last_post_at`, and
reading the actual newest reply may also consume the conversation-level unread
state.

```mermaid
flowchart TD
    A[Latest lower-edge-visible post] --> B{Thread view?}
    B -- no --> C{At root source tail?}
    C -- no --> D[Advance local cursor only]
    C -- yes --> E{DM/GM?}
    E -- no --> F[Mark channel viewed]
    E -- yes --> G{CRT root-count unread domain?}
    G -- yes --> H{Read post reaches last_root_post_at?}
    H -- yes --> F
    H -- no --> D
    G -- no --> I{Read post reaches last_post_at?}
    I -- yes --> F
    I -- no --> D
    B -- yes --> J{Authoritative thread tail?}
    J -- no --> D
    J -- yes --> K[Mark thread read]
    K --> L{DM/GM and replies belong to parent?}
    L -- yes --> M{Read post reaches last_post_at?}
    M -- yes --> F
    M -- no --> N[Thread acknowledgement only]
    L -- no --> N
```
"""
replace_once("docs/following-attention-read-tracking.md", old_doc, new_doc)
