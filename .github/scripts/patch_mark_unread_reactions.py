from pathlib import Path


def read(path):
    return Path(path).read_text()


def write(path, text):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing block in {path}: {old[:180]!r}")
    write(path, text.replace(old, new, 1))


def insert_before(path, marker, block):
    text = read(path)
    pos = text.find(marker)
    if pos < 0:
        raise SystemExit(f"missing marker in {path}: {marker!r}")
    write(path, text[:pos] + block + text[pos:])


# ---- Server set-unread request seam. ----
write("sources/backend/PostUnreadRequest.h", r'''#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

inline QString postUnreadPath(const QString& userId, const QString& postId)
{
    return QStringLiteral("users/") + userId + QStringLiteral("/posts/") + postId
        + QStringLiteral("/set_unread");
}

inline QJsonObject postUnreadPayload(bool collapsedThreadsSupported)
{
    return QJsonObject {{QStringLiteral("collapsed_threads_supported"),
                         collapsedThreadsSupported}};
}

} // namespace Mattermost
''')

# ---- Channel unread tracker must deliberately allow the read watermark to move backwards. ----
replace_once(
    "sources/backend/ChannelActivityTracker.h",
    "    void recordViewed(const QString& channelId, uint64_t viewedAt,\n"
    "                      uint64_t totalMessageCount, uint64_t totalRootMessageCount,\n"
    "                      bool hasTotalRootMessageCount);\n",
    "    void recordViewed(const QString& channelId, uint64_t viewedAt,\n"
    "                      uint64_t totalMessageCount, uint64_t totalRootMessageCount,\n"
    "                      bool hasTotalRootMessageCount);\n"
    "    /** Apply the authoritative response from POST /posts/{id}/set_unread. */\n"
    "    void markUnread(const QString& channelId, uint64_t lastViewedAt,\n"
    "                    uint64_t readMessageCount, uint64_t readRootMessageCount,\n"
    "                    bool hasReadRootMessageCount, uint64_t mentionCount,\n"
    "                    uint64_t rootMentionCount, bool hasRootMentionCount);\n")

insert_before(
    "sources/backend/ChannelActivityTracker.cpp",
    "void ChannelActivityTracker::setRecencyTimes(",
    r'''void ChannelActivityTracker::markUnread(const QString& channelId,
                                                uint64_t lastViewedAt,
                                                uint64_t readMessageCount,
                                                uint64_t readRootMessageCount,
                                                bool hasReadRootMessageCount,
                                                uint64_t mentionCount,
                                                uint64_t rootMentionCount,
                                                bool hasRootMentionCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;

    // Unlike ordinary membership refreshes, set_unread is explicitly allowed
    // to move the user's read watermark backwards.
    entry.lastViewedAt = lastViewedAt;
    entry.readMessageCount = readMessageCount;
    if (hasReadRootMessageCount) {
        entry.readRootMessageCount = readRootMessageCount;
        entry.hasReadRootMessageCount = true;
    }

    entry.mentionCount = mentionCount;
    entry.rootMentionCount = rootMentionCount;
    entry.hasRootMentionCount = hasRootMentionCount;
    entry.serverUnreadActivity = true;
    entry.runtimeUnreadActivity = false;
    entry.serverMentioned = collapsedThreadsEnabled && hasRootMentionCount
        ? rootMentionCount > 0
        : mentionCount > 0;
    entry.runtimeMentioned = false;
}

''')

# ---- Sidebar service owns the Mattermost set-unread REST call and applies its returned cursor. ----
replace_once(
    "sources/backend/SidebarService.h",
    "    void markChannelViewedLocally(const BackendChannel& channel);\n"
    "    void synchronizeChannelActivity();\n",
    "    void markChannelViewedLocally(const BackendChannel& channel);\n"
    "    void markPostUnread(const QString& postId, std::function<void(bool)> callback = {});\n"
    "    void synchronizeChannelActivity();\n")

text = read("sources/backend/SidebarService.cpp")
if '#include <QNetworkReply>\n' not in text:
    text = text.replace('#include <QJsonObject>\n', '#include <QJsonObject>\n#include <QNetworkReply>\n', 1)
if '#include "backend/PostUnreadRequest.h"\n' not in text:
    text = text.replace('#include "backend/NetworkRequest.h"\n', '#include "backend/NetworkRequest.h"\n#include "backend/PostUnreadRequest.h"\n', 1)
write("sources/backend/SidebarService.cpp", text)

insert_before(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::synchronizeChannelActivity()",
    r'''void SidebarService::markPostUnread(const QString& postId,
                                           std::function<void(bool)> callback)
{
    const QString userId = currentUserId();
    if (userId.isEmpty() || postId.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(postUnreadPath(userId, postId));
    httpConnector.post(
        request,
        QByteArrayCreator(postUnreadPayload(collapsedThreadsEnabled)),
        HttpResponseCallback([this, callback = std::move(callback)](
                                 QVariant status, const QJsonDocument& doc) mutable {
            if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
                if (callback) {
                    callback(false);
                }
                return;
            }

            const QJsonObject object = doc.object();
            const QString channelId = object.value(QStringLiteral("channel_id")).toString();
            if (channelId.isEmpty()) {
                if (callback) {
                    callback(false);
                }
                return;
            }

            const auto nonNegative = [&object](const char* name) -> uint64_t {
                const qint64 value = object.value(QString::fromLatin1(name))
                    .toVariant().toLongLong();
                return value > 0 ? static_cast<uint64_t>(value) : 0;
            };

            const bool wasMentioned = activityTracker.hasMention(channelId);
            activityTracker.markUnread(
                channelId,
                nonNegative("last_viewed_at"),
                nonNegative("msg_count"),
                nonNegative("msg_count_root"),
                object.contains(QStringLiteral("msg_count_root")),
                nonNegative("mention_count"),
                nonNegative("mention_count_root"),
                object.contains(QStringLiteral("mention_count_root")));

            const bool isMentioned = activityTracker.hasMention(channelId);
            if (wasMentioned != isMentioned) {
                emit channelMentionedChanged(channelId, isMentioned);
            }
            emit channelActivityChanged(channelId);
            if (callback) {
                callback(true);
            }
        }));
}

''')

# ---- Manual unread attention/resume state in the shared Following model. ----
replace_once(
    "sources/backend/FollowingModel.h",
    "#include <QObject>\n",
    "#include <QHash>\n#include <QObject>\n#include <QSet>\n")
replace_once(
    "sources/backend/FollowingModel.h",
    "    /** Optimistically acknowledge a followed thread and reconcile with CRT. */\n",
    "    /** Keep an explicit server-backed Mark as unread in Attention until it is read again. */\n"
    "    void markPostUnread(const QString& channelId, const QString& threadId,\n"
    "                        const QString& postId, uint64_t createAt);\n\n"
    "    /** Optimistically acknowledge a followed thread and reconcile with CRT. */\n")
replace_once(
    "sources/backend/FollowingModel.h",
    "    Backend& backend_;\n",
    "    struct ManualUnreadMarker {\n"
    "        QString channelId;\n"
    "        QString threadId;\n"
    "        QString postId;\n"
    "        uint64_t createAt = 0;\n"
    "    };\n\n"
    "    Backend& backend_;\n"
    "    QHash<QString, ManualUnreadMarker> manualUnreadMarkers_;\n"
    "    QSet<QString> manualAttentionKeys_;\n")

# local key helper
replace_once(
    "sources/backend/FollowingModel.cpp",
    "uint64_t nowMs()\n{\n    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());\n}\n",
    "uint64_t nowMs()\n{\n    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());\n}\n\n"
    "QString manualUnreadKey(const QString& channelId, const QString& threadId)\n"
    "{\n"
    "    return threadId.isEmpty()\n"
    "        ? QStringLiteral(\"c:\") + channelId\n"
    "        : QStringLiteral(\"t:\") + channelId + QLatin1Char(':') + threadId;\n"
    "}\n")

replace_once(
    "sources/backend/FollowingModel.cpp",
    "    connect(&backend_, &Backend::onChannelViewed, this,\n"
    "            [this](const BackendChannel& channel) {\n"
    "        clearSyntheticMentions(channel.id);\n",
    "    connect(&backend_, &Backend::onChannelViewed, this,\n"
    "            [this](const BackendChannel& channel) {\n"
    "        const QString key = manualUnreadKey(channel.id, QString());\n"
    "        manualUnreadMarkers_.remove(key);\n"
    "        manualAttentionKeys_.remove(key);\n"
    "        clearSyntheticMentions(channel.id);\n")

# Replace syncConversations channel filtering/state block.
replace_once(
    "sources/backend/FollowingModel.cpp",
    r'''        if (!channel
            || (channel->type != BackendChannel::directChannel
                && channel->type != BackendChannel::groupChannel)) {
            continue;
        }

        const bool unread = sidebar.isChannelUnread(*channel);
        const bool mentioned = sidebar.hasUnreadMention(channel->id);
        const bool muted = sidebar.isChannelMuted(*channel);
        if (muted || (!unread && !mentioned)) {
            continue;
        }

        Entry entry;
''',
    r'''        if (!channel) {
            continue;
        }

        const QString manualKey = manualUnreadKey(channel->id, QString());
        const bool manualAttention = manualAttentionKeys_.contains(manualKey);
        const bool conversation = channel->type == BackendChannel::directChannel
            || channel->type == BackendChannel::groupChannel;
        if (!conversation && !manualAttention) {
            continue;
        }

        const bool serverUnread = sidebar.isChannelUnread(*channel);
        const bool mentioned = sidebar.hasUnreadMention(channel->id);
        const bool muted = sidebar.isChannelMuted(*channel);
        if (!manualAttention && (muted || (!serverUnread && !mentioned))) {
            continue;
        }

        Entry entry;
''')
replace_once(
    "sources/backend/FollowingModel.cpp",
    "        entry.unread = unread;\n"
    "        entry.mentioned = mentioned;\n"
    "        entry.muted = false;\n",
    "        entry.unread = serverUnread || manualAttention;\n"
    "        entry.mentioned = mentioned;\n"
    "        entry.muted = manualAttention ? false : muted;\n")
replace_once(
    "sources/backend/FollowingModel.cpp",
    "        if (entry.lastReplyAt == 0) {\n"
    "            entry.lastReplyAt = channel->last_post_at;\n"
    "        }\n"
    "        noteAttentionTransition(entry, wasAttention);\n",
    "        if (entry.lastReplyAt == 0) {\n"
    "            entry.lastReplyAt = channel->last_post_at;\n"
    "        }\n"
    "        const auto manualIt = manualUnreadMarkers_.constFind(manualKey);\n"
    "        if (manualIt != manualUnreadMarkers_.cend()) {\n"
    "            entry.resumeState = ResumeState::FirstUnread;\n"
    "            entry.firstUnreadPostId = manualIt->postId;\n"
    "            entry.readThroughPostId.clear();\n"
    "            entry.readThroughCreateAt = 0;\n"
    "        }\n"
    "        noteAttentionTransition(entry, wasAttention);\n")

# Add markPostUnread implementation before scheduleThreadRefresh.
insert_before(
    "sources/backend/FollowingModel.cpp",
    "void FollowingModel::scheduleThreadRefresh()",
    r'''void FollowingModel::markPostUnread(const QString& channelId,
                                           const QString& threadId,
                                           const QString& postId,
                                           uint64_t createAt)
{
    if (channelId.isEmpty() || postId.isEmpty()) {
        return;
    }

    const QString key = manualUnreadKey(channelId, threadId);
    manualUnreadMarkers_.insert(
        key, ManualUnreadMarker {channelId, threadId, postId, createAt});
    manualAttentionKeys_.insert(key);

    if (threadId.isEmpty()) {
        syncConversations();
        emit changed();
        return;
    }

    Entry* entry = findThreadMutable(threadId);
    if (!entry) {
        Entry created;
        created.kind = Kind::Thread;
        created.channelId = channelId;
        created.threadId = threadId;
        if (BackendChannel* channel = backend_.getStorage().getChannelById(channelId)) {
            created.teamId = channel->team ? channel->team->id : QString();
        }
        entries_.push_back(std::move(created));
        entry = &entries_.last();
    }

    const bool wasAttention = entry->requiresAttention();
    entry->unreadReplies = std::max(1, entry->unreadReplies);
    entry->resumeState = ResumeState::FirstUnread;
    entry->firstUnreadPostId = postId;
    entry->readThroughPostId.clear();
    entry->readThroughCreateAt = 0;
    entry->lastReplyAt = std::max(entry->lastReplyAt, createAt);
    entry->muted = false;
    noteAttentionTransition(*entry, wasAttention);
    emit changed();
}

''')

# Protect thread snapshots from a stale response predating set_unread.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "        if (readPending) {\n",
    "        const QString manualKey = manualUnreadKey(entry.channelId, entry.threadId);\n"
    "        const bool manualAttention = manualAttentionKeys_.contains(manualKey);\n"
    "        const auto manualIt = manualUnreadMarkers_.constFind(manualKey);\n"
    "        if (manualAttention) {\n"
    "            entry.unreadReplies = std::max(1, entry.unreadReplies);\n"
    "            if (manualIt != manualUnreadMarkers_.cend()) {\n"
    "                entry.resumeState = ResumeState::FirstUnread;\n"
    "                entry.firstUnreadPostId = manualIt->postId;\n"
    "                entry.readThroughPostId.clear();\n"
    "                entry.readThroughCreateAt = 0;\n"
    "            }\n"
    "        }\n\n"
    "        if (readPending) {\n")

# Consume only the manual marker when its lower edge is genuinely read; keep explicit attention until ack.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "    Entry* entry = findEntryMutable(channelId, threadId);\n"
    "    if (!entry || post.id.isEmpty()) {\n",
    "    Entry* entry = findEntryMutable(channelId, threadId);\n"
    "    if (!entry || post.id.isEmpty()) {\n")
# Insert after null guard block by matching the comment following it.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "        return;\n"
    "    }\n\n"
    "    const bool sameBoundary = entry->readThroughPostId == post.id;\n",
    "        return;\n"
    "    }\n\n"
    "    const QString manualKey = manualUnreadKey(channelId, threadId);\n"
    "    const auto manualIt = manualUnreadMarkers_.constFind(manualKey);\n"
    "    if (manualIt != manualUnreadMarkers_.cend()\n"
    "        && (post.id == manualIt->postId\n"
    "            || isAfter(post.create_at, post.id, manualIt->createAt, manualIt->postId))) {\n"
    "        manualUnreadMarkers_.remove(manualKey);\n"
    "    }\n\n"
    "    const bool sameBoundary = entry->readThroughPostId == post.id;\n")

# Clearing a thread at the authoritative tail also clears its explicit manual-attention latch.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "    if (Entry* entry = findThreadMutable(threadId)) {\n"
    "        entry->unreadReplies = 0;\n",
    "    const QString manualKey = manualUnreadKey(QString(), threadId);\n"
    "    for (auto it = manualAttentionKeys_.begin(); it != manualAttentionKeys_.end();) {\n"
    "        if (it->endsWith(QLatin1Char(':') + threadId)) {\n"
    "            manualUnreadMarkers_.remove(*it);\n"
    "            it = manualAttentionKeys_.erase(it);\n"
    "        } else {\n"
    "            ++it;\n"
    "        }\n"
    "    }\n\n"
    "    if (Entry* entry = findThreadMutable(threadId)) {\n"
    "        entry->unreadReplies = 0;\n")
# Avoid an unused variable introduced above; key is not needed because channel is unknown here.
replace_once(
    "sources/backend/FollowingModel.cpp",
    "    const QString manualKey = manualUnreadKey(QString(), threadId);\n"
    "    for (auto it = manualAttentionKeys_.begin(); it != manualAttentionKeys_.end();) {\n",
    "    for (auto it = manualAttentionKeys_.begin(); it != manualAttentionKeys_.end();) {\n")

# ---- Viewport re-entry gate. ----
write("sources/chat-area/ManualUnreadVisibilityGate.h", r'''#pragma once

#include <QString>
#include <utility>

namespace Mattermost {

/**
 * Prevents a freshly manual-unread post from being immediately consumed merely
 * because its lower edge was already visible when the command was issued.
 */
class ManualUnreadVisibilityGate
{
public:
    enum class Phase {
        Idle,
        WaitForExit,
        WaitForEntry,
    };

    void markUnread(QString postId, bool lowerEdgeVisible)
    {
        postId_ = std::move(postId);
        phase_ = postId_.isEmpty()
            ? Phase::Idle
            : (lowerEdgeVisible ? Phase::WaitForExit : Phase::WaitForEntry);
    }

    /** Returns true while the current read-progress pass must be blocked. */
    bool update(bool lowerEdgeVisible)
    {
        if (phase_ == Phase::Idle) {
            return false;
        }
        if (phase_ == Phase::WaitForExit) {
            if (!lowerEdgeVisible) {
                phase_ = Phase::WaitForEntry;
            }
            return true;
        }
        if (!lowerEdgeVisible) {
            return true;
        }
        clear();
        return false;
    }

    void clear()
    {
        postId_.clear();
        phase_ = Phase::Idle;
    }

    bool active() const { return phase_ != Phase::Idle; }
    const QString& postId() const { return postId_; }
    Phase phase() const { return phase_; }

private:
    QString postId_;
    Phase phase_ = Phase::Idle;
};

} // namespace Mattermost
''')

replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "#include \"AbstractPostSource.h\"\n",
    "#include \"AbstractPostSource.h\"\n#include \"ManualUnreadVisibilityGate.h\"\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    void updateReadCursorFromViewport();\n",
    "    void updateReadCursorFromViewport();\n"
    "    void markPostUnread(const QString& postId);\n"
    "    bool isPostLowerEdgeVisible(const QString& postId) const;\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    bool _initialScrollBarPulsePending = true;\n",
    "    bool _initialScrollBarPulsePending = true;\n"
    "    ManualUnreadVisibilityGate manualUnreadGate_;\n")

# Connect menu request to the list owner.
replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    connect(widget, &PostWidget::wholeMessageSelectionToggled,\n"
    "            this, [this](const QString& id, bool selected) {\n"
    "        setMessagePostSelected(id, selected);\n"
    "    });\n",
    "    connect(widget, &PostWidget::wholeMessageSelectionToggled,\n"
    "            this, [this](const QString& id, bool selected) {\n"
    "        setMessagePostSelected(id, selected);\n"
    "    });\n"
    "    connect(widget, &PostWidget::markUnreadRequested,\n"
    "            this, &ChatLogWidget::markPostUnread);\n")

# Gate read progress before computing a new high-water mark.
replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    // Reading is a viewport fact, not a navigation fact. Among concrete posts\n",
    "    if (manualUnreadGate_.active()) {\n"
    "        const QString gatedPostId = manualUnreadGate_.postId();\n"
    "        const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);\n"
    "        if (manualUnreadGate_.update(lowerEdgeVisible)) {\n"
    "            qCDebug(lcTimelineTrace).nospace()\n"
    "                << \"READ_CURSOR_MANUAL_UNREAD_BLOCK list=\"\n"
    "                << static_cast<const void*>(this)\n"
    "                << \" post=\" << gatedPostId\n"
    "                << \" lowerEdgeVisible=\" << lowerEdgeVisible;\n"
    "            return;\n"
    "        }\n"
    "    }\n\n"
    "    // Reading is a viewport fact, not a navigation fact. Among concrete posts\n")

# Add mark/read-edge helpers before clearNavigationLock.
insert_before(
    "sources/chat-area/ChatLogWidget.cpp",
    "void ChatLogWidget::clearNavigationLock()",
    r'''bool ChatLogWidget::isPostLowerEdgeVisible(const QString& postId) const
{
    if (!postSource || postId.isEmpty() || viewport()->height() <= 0) {
        return false;
    }
    const int index = postSource->indexOfPost(postId);
    QWidget* widget = index >= 0 ? itemWidget(index) : nullptr;
    if (!widget) {
        return false;
    }
    const int bottom = widget->y() + widget->height();
    return bottom > 0 && bottom <= viewport()->height();
}

void ChatLogWidget::markPostUnread(const QString& postId)
{
    if (!backend || !chatArea || !postSource || postId.isEmpty()) {
        return;
    }
    const int index = postSource->indexOfPost(postId);
    BackendPost* post = index >= 0 ? postSource->postAt(index) : nullptr;
    if (!post) {
        return;
    }

    const QString channelId = chatArea->getChannel().id;
    const QString threadId = chatArea->isThread ? chatArea->root_id : QString();
    const uint64_t createAt = post->create_at;
    manualUnreadGate_.markUnread(postId, isPostLowerEdgeVisible(postId));

    QPointer<ChatLogWidget> guard(this);
    SidebarService::instance(*backend).markPostUnread(
        postId,
        [guard, channelId, threadId, postId, createAt](bool success) {
            if (!guard || !guard->backend) {
                return;
            }
            if (!success) {
                if (guard->manualUnreadGate_.postId() == postId) {
                    guard->manualUnreadGate_.clear();
                    guard->scheduleReadCursorUpdate();
                }
                return;
            }

            auto& following = FollowingModel::instance(*guard->backend);
            following.markPostUnread(channelId, threadId, postId, createAt);
            if (!threadId.isEmpty()) {
                following.refreshThreads();
            }
            guard->scheduleReadCursorUpdate();
        });
}

''')

# ---- Context action + better heart anchor. ----
replace_once(
    "sources/chat-area/post/PostWidget.h",
    "    void wholeMessageSelectionToggled(const QString& postId, bool selected);\n",
    "    void wholeMessageSelectionToggled(const QString& postId, bool selected);\n"
    "    void markUnreadRequested(const QString& postId);\n")

replace_once(
    "sources/chat-area/post/PostWidget.cpp",
    "    const int x = 4;\n"
    "    const int y = std::max(4, height() - reactionAffordance_->height() - 6);\n"
    "    reactionAffordance_->move(x, y);\n",
    "    int x = 4;\n"
    "    int y = std::max(4, height() - reactionAffordance_->height() - 6);\n"
    "    if (threadSummary && threadSummary->isVisible()) {\n"
    "        const QPoint threadTopLeft = threadSummary->mapTo(this, QPoint(0, 0));\n"
    "        x = std::max(4, threadTopLeft.x() - reactionAffordance_->width() - 4);\n"
    "        y = threadTopLeft.y()\n"
    "            + (threadSummary->height() - reactionAffordance_->height()) / 2;\n"
    "    }\n"
    "    reactionAffordance_->move(x, std::max(2, y));\n")

# Insert Mark as unread just before Save message.
replace_once(
    "sources/chat-area/post/PostWidget.cpp",
    "    QAction* saveAction = menu.addAction(icon(QStringLiteral(\":/icons/bookmark\")),\n"
    "                                         tr(\"Save message\"));\n",
    "    QAction* unreadAction = menu.addAction(icon(QStringLiteral(\":/icons/unread\")),\n"
    "                                           tr(\"Mark as unread\"));\n"
    "    connect(unreadAction, &QAction::triggered, this, [this] {\n"
    "        emit markUnreadRequested(post.id);\n"
    "    });\n\n"
    "    QAction* saveAction = menu.addAction(icon(QStringLiteral(\":/icons/bookmark\")),\n"
    "                                         tr(\"Save message\"));\n")

# ---- Reaction chips: child labels must not swallow the parent's click. ----
replace_once(
    "sources/chat-area/post/reactions/PostReaction.cpp",
    "    ui_->setupUi(this);\n\n"
    "    const QFont reactionFont = EmojiPresentation::fontForMode(\n",
    "    ui_->setupUi(this);\n"
    "    // The whole chip is one click target. QLabel children otherwise win\n"
    "    // hit-testing, which made clicks on later/more tightly packed chips\n"
    "    // appear to do nothing.\n"
    "    ui_->emoji->setAttribute(Qt::WA_TransparentForMouseEvents);\n"
    "    ui_->count->setAttribute(Qt::WA_TransparentForMouseEvents);\n\n"
    "    const QFont reactionFont = EmojiPresentation::fontForMode(\n")

# ---- Icon resource. ----
write("img/unread.svg", r'''<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
  <rect x="3.5" y="5" width="17" height="14" rx="2"/>
  <path d="m4.5 7 7.5 6 7.5-6"/>
  <path d="M3 3h6"/>
</svg>
''')
replace_once(
    "resource.qrc",
    "        <file alias=\"link\">img/link.svg</file>\n",
    "        <file alias=\"link\">img/link.svg</file>\n"
    "        <file alias=\"unread\">img/unread.svg</file>\n")

# ---- Regression tests. ----
write("tests/ManualUnreadVisibilityGateTest.cpp", r'''#include <QtTest>

#include "chat-area/ManualUnreadVisibilityGate.h"

using namespace Mattermost;

class ManualUnreadVisibilityGateTest : public QObject
{
    Q_OBJECT

private slots:
    void visibleMarkRequiresExitAndReentry()
    {
        ManualUnreadVisibilityGate gate;
        gate.markUnread(QStringLiteral("post"), true);

        QVERIFY(gate.update(true));
        QVERIFY(gate.update(true));
        QVERIFY(gate.update(false));
        QCOMPARE(gate.phase(), ManualUnreadVisibilityGate::Phase::WaitForEntry);
        QVERIFY(gate.update(false));
        QVERIFY(!gate.update(true));
        QVERIFY(!gate.active());
    }

    void initiallyHiddenMarkReadsOnFirstEntry()
    {
        ManualUnreadVisibilityGate gate;
        gate.markUnread(QStringLiteral("post"), false);

        QVERIFY(gate.update(false));
        QVERIFY(!gate.update(true));
        QVERIFY(!gate.active());
    }
};

QTEST_MAIN(ManualUnreadVisibilityGateTest)
#include "ManualUnreadVisibilityGateTest.moc"
''')

write("tests/PostUnreadRequestTest.cpp", r'''#include <QtTest>

#include "backend/PostUnreadRequest.h"

using namespace Mattermost;

class PostUnreadRequestTest : public QObject
{
    Q_OBJECT

private slots:
    void pathAndCrtCapabilityMatchMattermostApi()
    {
        QCOMPARE(postUnreadPath(QStringLiteral("user"), QStringLiteral("post")),
                 QStringLiteral("users/user/posts/post/set_unread"));
        QCOMPARE(postUnreadPayload(true).value(QStringLiteral("collapsed_threads_supported")).toBool(),
                 true);
        QCOMPARE(postUnreadPayload(false).value(QStringLiteral("collapsed_threads_supported")).toBool(),
                 false);
    }
};

QTEST_MAIN(PostUnreadRequestTest)
#include "PostUnreadRequestTest.moc"
''')

# Add tracker regression before class end.
replace_once(
    "tests/ChannelActivityTrackerTest.cpp",
    "    void ownPostDoesNotCreateUnreadState()\n",
    r'''    void explicitMarkUnreadCanMoveWatermarkBackwards()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 5000, 12, 8);
        synchronize(tracker, 5000, 12, 8, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.markUnread(QStringLiteral("channel"), 2999,
                           7, 5, true, 0, 0, true);
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(2999));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 5000, 12, 8, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(5000));
    }

    void ownPostDoesNotCreateUnreadState()
''')

write("tests/PostReactionInteractionTest.cpp", r'''#include <algorithm>

#include <QApplication>
#include <QSignalSpy>
#include <QtTest>

#include "backend/Backend.h"
#include "chat-area/post/reactions/PostReaction.h"
#include "chat-area/post/reactions/PostReactionList.h"

using namespace Mattermost;

class PostReactionInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void secondChipIsOneClickableHitTarget()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("thumbsup"), QString::fromUtf8("👍"),
                         BackendPostReaction {QStringLiteral("Alice")});
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Bob")});
        list.resize(220, 32);
        list.show();
        QTest::qWaitForWindowExposed(&list);
        QApplication::processEvents();

        auto chips = list.findChildren<PostReaction*>();
        QCOMPARE(chips.size(), 2);
        std::sort(chips.begin(), chips.end(), [](PostReaction* lhs, PostReaction* rhs) {
            return lhs->mapToGlobal(QPoint()).x() < rhs->mapToGlobal(QPoint()).x();
        });
        PostReaction* second = chips.at(1);
        const QPoint globalCenter = second->mapToGlobal(second->rect().center());
        QWidget* hit = QApplication::widgetAt(globalCenter);
        QCOMPARE(hit, static_cast<QWidget*>(second));

        QSignalSpy spy(&list, &PostReactionList::reactionClicked);
        QTest::mouseClick(hit, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("eyes"));
    }
};

QTEST_MAIN(PostReactionInteractionTest)
#include "PostReactionInteractionTest.moc"
''')

# CMake registration.
with open("tests/CMakeLists.txt", "a") as f:
    f.write(r'''

add_executable(manual-unread-visibility-gate-test ManualUnreadVisibilityGateTest.cpp)
target_include_directories(manual-unread-visibility-gate-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(manual-unread-visibility-gate-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME manual-unread-visibility-gate-test COMMAND manual-unread-visibility-gate-test)

add_executable(post-unread-request-test PostUnreadRequestTest.cpp)
target_include_directories(post-unread-request-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(post-unread-request-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME post-unread-request-test COMMAND post-unread-request-test)

if(TARGET mattermost-core)
    add_executable(post-reaction-interaction-test PostReactionInteractionTest.cpp)
    target_link_libraries(post-reaction-interaction-test PRIVATE mattermost-core Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME post-reaction-interaction-test COMMAND post-reaction-interaction-test)
    set_tests_properties(post-reaction-interaction-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
''')
