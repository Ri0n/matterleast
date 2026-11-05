#pragma once

#include <optional>
#include <memory>

#include <cstdint>

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include "AbstractPostSource.h"
#include "ManualUnreadVisibilityGate.h"
#include "PostListWidget.h"

class QFrame;
class QLabel;
class QPushButton;
class QResizeEvent;

namespace Mattermost {

class Backend;
class BackendPost;
class AttentionFlashLifetime;
class ChatArea;
class PostWidget;
struct PendingPost;

/** Mattermost timeline behavior layered on the shared post-list presentation. */
class ChatLogWidget : public PostListWidget
{
    Q_OBJECT
public:
    explicit ChatLogWidget(QWidget* parent = nullptr);
    ~ChatLogWidget() override;

    void configure(Backend& backend, ChatArea& chatArea);
    void setSource(AbstractPostSource* source);
    AbstractPostSource* source() const { return postSource; }

    PostWidget* findPost(const QString& postId) const;

    struct SemanticViewportBookmark {
        QString postId;
        SemanticAnchorPoint point = SemanticAnchorPoint::ItemCenter;
        qreal fraction = 0;

        bool isValid() const { return !postId.isEmpty(); }
    };

    // Runtime-only per-tab bookmark. At the live edge, anchor the bottom-most
    // post by its bottom. Else keep a concrete post's center relative to the
    // viewport center (including signed offsets for very long messages).
    bool captureSemanticViewportBookmark(SemanticViewportBookmark& bookmark) const;
    bool restoreSemanticViewportBookmark(const SemanticViewportBookmark& bookmark);

    /** Capture a semantic post near the viewport centre for inactive-page restore. */
    bool captureViewportBookmark(QString& postId, qreal* topFraction = nullptr) const;

    /** Restore an inactive-page bookmark without the navigation highlight animation. */
    bool restoreViewportBookmark(const QString& postId,
                                 std::optional<qreal> topFraction = std::nullopt);

    bool ensurePostVisible(const QString& postId,
                           Alignment alignment = Alignment::EnsureVisible);
    void highlightPost(const QString& postId);

    /** Flash only currently rendered, unread Attention posts in this viewport. */
    void flashVisibleAttention();
    void refreshPost(const QString& postId);

    /** Keep the locally confirmed outgoing post at the live edge of the log. */
    void followOwnPost(const QString& postId);

    /** Consume transient mention guidance without changing source row indices. */
    bool receiveEphemeralMention(const QJsonObject& ephemeralPost);

    /**
     * Re-evaluate read progress from the current viewport. This never treats the
     * caller/navigation action itself as a read; the lower edge of a concrete
     * post must actually be inside the active viewport.
     */
    void refreshReadState();

    /**
     * Keep a semantic post target anchored while its provisional logical index
     * is replaced by authoritative source data. ChatLogWidget owns only post
     * identity; LongListWidget owns the actual viewport lock and all scroll math.
     */
    bool lockNavigationToPost(const QString& postId,
                              Alignment alignment = Alignment::Center,
                              int quietPeriodMs = 2000,
                              std::optional<SemanticViewportBookmark> semanticBookmark = std::nullopt);
    void clearNavigationLock();

    /** Composer up-arrow action; no QListWidgetItem leaks through this API. */
    bool editLastOwnPost();
    void postEditFinished();

    bool isMessageSelectionMode() const { return messageSelectionMode_; }
    void beginMessageSelectionDrag(const QString& anchorPostId, const QString& currentPostId);
    void updateMessageSelectionDrag(const QString& currentPostId);
    void finishMessageSelectionDrag();
    void cancelMessageSelection();

signals:
    void postEditInitiated(BackendPost& post);

protected:
    QWidget* createItemWidget(int index) override;
    QString itemIdentity(const QWidget* widget) const override;
    int indexOfItemIdentity(const QString& identity) const override;
    bool isModelItemAvailable(int index) const override;
    void destroyItemWidget(int index, QWidget* widget) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    static AbstractPostSource::RequestReason toSourceReason(RequestReason reason);
    void reconnectSource();
    bool restoreNavigationTarget();
    bool finalizeNavigationLock();
    void scheduleNavigationFinalize();
    void scheduleReadCursorUpdate();
    void updateReadCursorFromVisibility();
    void refreshUnreadEdgeBadge();
    uint64_t unreadEdgeCount() const;
    int firstUnreadEdgeIndex() const;
    bool navigateToUnreadEdge();
    void finalizeUnreadEdgeTarget();
    void queueReadProgress(const BackendPost& post, int index);
    void flushReadProgress();
    void acknowledgeVisibleLiveThreadPost(const BackendPost& post);
    bool shouldContinueAuthorRun(int index) const;
    void applyAuthorRunPresentation(int index, PostWidget& widget);
    void refreshMaterializedAuthorRuns();
    void scheduleAuthorRunRefresh();
    void markPostUnread(const QString& postId);
    bool isPostLowerEdgeVisible(const QString& postId) const;
    void setMessageSelectionRange(const QString& currentPostId);
    void setMessagePostSelected(const QString& postId, bool selected);
    void applyMessageSelectionVisuals();
    void cacheSelectedPost(const QString& postId);
    void ensureSelectionToolbar();
    void updateSelectionToolbar();
    void positionSelectionToolbar();
    void copySelectedPosts();
    void deleteSelectedOwnPosts();
    void attachEphemeralMention(PostWidget& widget);
    bool isLocalPresentationIndex(int index) const;
    bool isLocalPresentationId(const QString& postId) const;
    void applyPendingPresentation(PostWidget& widget,
                                  const PendingPost& pending);

    Backend* backend = nullptr;
    ChatArea* chatArea = nullptr;
    QPointer<AbstractPostSource> postSource;
    QPointer<PostWidget> editedPostWidget;
    // Ephemeral post.create_at = original post.create_at + 1 in Mattermost.
    // Keep a bounded session-only overlay map, never in PostRepository.
    QHash<quint64, QJsonObject> ephemeralMentions_;
    // Destroyed before LongListWidget tears down materialized post widgets.
    std::unique_ptr<AttentionFlashLifetime> _attentionFlashLifetime;
    QVector<QMetaObject::Connection> sourceConnections;

    QString navigationPostId;
    QString pendingHighlightPostId;
    int navigationLogicalIndex = -1;
    Alignment navigationAlignment = Alignment::Center;
    std::optional<SemanticViewportBookmark> _navigationSemanticBookmark;
    int navigationQuietPeriodMs = 2000;
    bool navigationLockPending = false;
    bool navigationRecenterPending = false;
    bool readCursorUpdatePending_ = false;
    bool authorRunRefreshPending_ = false;
    bool _initialScrollBarPulsePending = true;
    ManualUnreadVisibilityGate manualUnreadGate_;
    QString manualUnreadHighWaterPostId_;
    uint64_t manualUnreadHighWaterCreateAt_ = 0;
    bool manualUnreadExitedViewport_ = false;

    // UI navigation state. Only a genuine upward scroll resets the shortcut
    // from a consumed automatic unread head to the live edge.
    bool autoUnreadHeadConsumed_ = false;
    bool manualUnreadTail_ = false;
    int previousScrollValue_ = 0;
    int pendingUnreadIndex_ = -1;
    uint64_t observedReadAt_ = 0;
    QString observedReadPostId_;
    QTimer* readProgressTimer_ = nullptr;
    bool readProgressInFlight_ = false;
    uint64_t readProgressSentAt_ = 0;
    int readProgressGeneration_ = 0;

    bool messageSelectionMode_ = false;
    bool messageSelectionDragActive_ = false;
    QString messageSelectionAnchorPostId_;
    QSet<QString> selectedPostIds_;
    QSet<QString> selectedOwnPostIds_;
    QHash<QString, QString> selectedFormattedPosts_;
    QFrame* selectionToolbar_ = nullptr;
    QLabel* selectionCountLabel_ = nullptr;
    QPushButton* selectionDeleteButton_ = nullptr;
    QPushButton* selectionCopyButton_ = nullptr;
};

} // namespace Mattermost
