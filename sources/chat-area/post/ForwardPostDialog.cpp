#include "ForwardPostDialog.h"

#include <algorithm>
#include <utility>

#include <QAbstractItemView>
#include <QColor>
#include <QDialogButtonBox>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QVector>
#include <QVBoxLayout>

#include "PostForwardPolicy.h"
#include "Settings.h"
#include "options/MLOptions.h"
#include "backend/Backend.h"
#include "backend/PostCreateService.h"
#include "PostPermalinkService.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "chat-area/QuotedPostPreview.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int MaxVisibleProfileRequests = 12;
constexpr int MaxRemoteUserResults = 25;
constexpr int RemoteSearchDelayMs = 250;
constexpr int UserIdRole = Qt::UserRole + 2;
constexpr int RemoteUserRole = Qt::UserRole + 3;

struct Destination {
    QString id;
    QString label;
    QString search;
    QString userId;
    bool unresolved = false;
};

QString channelLabel(const BackendChannel& channel)
{
    const QString name = channel.display_name.isEmpty()
        ? channel.name : channel.display_name;
    switch (channel.type) {
    case BackendChannel::publicChannel:
        return (channel.team ? channel.team->display_name + QStringLiteral(" · ") : QString())
            + QStringLiteral("#") + name;
    case BackendChannel::privateChannel:
        return (channel.team ? channel.team->display_name + QStringLiteral(" · ") : QString())
            + QStringLiteral("🔒 ") + name;
    case BackendChannel::directChannel:
        return QStringLiteral("@") + name;
    case BackendChannel::groupChannel:
        return QStringLiteral("Group · ") + name;
    default:
        return name;
    }
}

QString labelForChannel(const BackendChannel& channel, const Storage& storage)
{
    if (channel.type == BackendChannel::directChannel) {
        if (const BackendUser* user = storage.getUserById(channel.name)) {
            const QString name = user->getDisplayName();
            if (!name.isEmpty()) {
                return QStringLiteral("@") + name;
            }
        }
    }
    return channelLabel(channel);
}

} // namespace

ForwardPostDialog::ForwardPostDialog(
    Backend& backend, const BackendPost& post, QWidget* parent)
    : QDialog(parent)
    , backend_(backend)
    , sourceChannelId_(post.channel_id)
    , sourcePostId_(post.id)
    , sourceMessage_(post.message)
    , sourceAuthorId_(post.user_id)
    , sourceHasFileAttachments_(!post.files.empty())
    , usePermalink_(MLOptions::instance()->optionObject<bool>(
          FORWARD_WITH_PERMALINK, FORWARD_WITH_PERMALINK_DEFAULT)->value().toBool())
{
    setObjectName(QStringLiteral("forwardPostDialog"));
    setWindowTitle(tr("Forward message"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(540, 520);

    const BackendChannel* source =
        backend_.getStorage().getChannelById(sourceChannelId_);
    sourceType_ = source ? source->type : BackendChannel::unknown;
    sourceTeamSlug_ = PostPermalinkService::teamSlugForPost(backend_, post);
    sourceTeamId_ = source && source->team ? source->team->id
                                           : backend_.getCurrentTeamContextId();

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    if (sourceType_ == BackendChannel::publicChannel) {
        auto* heading = new QLabel(tr("Forward to"), this);
        layout->addWidget(heading);

        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("forwardDestinationFilter"));
        search_->setPlaceholderText(tr("Filter channels and conversations"));
        layout->addWidget(search_);
    } else {
        auto* notice = new QLabel(
            tr("Messages from private channels and direct conversations "
               "can only be forwarded within their original conversation."),
            this);
        notice->setObjectName(QStringLiteral("forwardPrivacyNotice"));
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }

    destinations_ = new QListWidget(this);
    destinations_->setObjectName(QStringLiteral("forwardDestinations"));
    destinations_->setSelectionMode(QAbstractItemView::SingleSelection);
    destinations_->setMinimumHeight(110);
    destinations_->viewport()->installEventFilter(this);
    connect(destinations_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { resolveVisibleDirectNames(); });
    layout->addWidget(destinations_, 1);

    auto* commentLabel = new QLabel(tr("Comment (optional)"), this);
    layout->addWidget(commentLabel);

    comment_ = new QPlainTextEdit(this);
    comment_->setObjectName(QStringLiteral("forwardComment"));
    comment_->setPlaceholderText(tr("Add a comment to the forwarded message"));
    comment_->setMaximumHeight(110);
    comment_->setMinimumHeight(65);
    layout->addWidget(comment_);

    auto* previewHeading = new QLabel(tr("Message preview"), this);
    layout->addWidget(previewHeading);
    auto* preview = new QuotedPostPreview(this, 3);
    preview->setObjectName(QStringLiteral("forwardMessagePreview"));
    const QString author = post.getDisplayAuthorName();
    preview->setPreview(
        author.isEmpty() ? tr("Message") : author,
        post.message,
        !post.files.empty());
    layout->addWidget(preview);

    auto* origin = new QLabel(
        tr("Originally posted in %1").arg(
            source ? channelLabel(*source) : tr("the original conversation")),
        this);
    origin->setObjectName(QStringLiteral("forwardOrigin"));
    origin->setWordWrap(true);
    layout->addWidget(origin);

    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("forwardError"));
    error_->setWordWrap(true);
    QPalette errorPalette = error_->palette();
    errorPalette.setColor(QPalette::WindowText, QColor(175, 48, 48));
    error_->setPalette(errorPalette);
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    forwardButton_ = buttons->addButton(tr("Forward"), QDialogButtonBox::AcceptRole);
    forwardButton_->setObjectName(QStringLiteral("forwardSubmit"));
    forwardButton_->setDefault(true);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(forwardButton_, &QPushButton::clicked, this, [this] { submit(); });
    connect(destinations_, &QListWidget::currentRowChanged,
            this, [this](int) { updateSubmitEnabled(); });
    connect(destinations_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { submit(); });
    if (search_) {
        connect(search_, &QLineEdit::textChanged,
                this, [this](const QString& filter) {
            filterDestinations(filter);
        });
    }
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(RemoteSearchDelayMs);
    connect(&searchTimer_, &QTimer::timeout, this,
            [this] { searchRemoteUsers(); });
    connect(&UserProfileService::instance(backend_),
            &UserProfileService::profileResolved,
            this, [this](const QString& userId) { updateResolvedUser(userId); });

    populateDestinations();
    updateSubmitEnabled();
    if (search_) {
        search_->setFocus();
    }
}

bool ForwardPostDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (destinations_ && watched == destinations_->viewport() && event
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        resolveVisibleDirectNames();
    }
    return QDialog::eventFilter(watched, event);
}

void ForwardPostDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    resolveVisibleDirectNames();
}

void ForwardPostDialog::populateDestinations()
{
    // Keep a stable row order for this dialog's lifetime. Unknown DM names go
    // after already-readable rows; when a profile resolves, only that item's
    // text changes, never its position or the selection/scroll offset.
    const Storage& storage = backend_.getStorage();
    QVector<Destination> rows;
    rows.reserve(storage.channels.size());
    for (BackendChannel* channel : storage.channels) {
        if (!channel || channel->delete_at || channel->id.isEmpty()
            || !canForwardPostTo(sourceType_, sourceChannelId_, channel->id)) {
            continue;
        }
        if (channel->type != BackendChannel::publicChannel
            && channel->type != BackendChannel::privateChannel
            && channel->type != BackendChannel::directChannel
            && channel->type != BackendChannel::groupChannel) {
            continue;
        }

        Destination dest;
        dest.id = channel->id;
        dest.label = labelForChannel(*channel, storage);
        if (channel->type == BackendChannel::directChannel) {
            dest.userId = channel->name;
            const BackendUser* user = storage.getUserById(dest.userId);
            dest.unresolved = !user;
            if (user) {
                dest.search = user->username;
            }
        }
        dest.search += QLatin1Char(' ') + dest.label
            + QLatin1Char(' ') + channel->name;
        rows.push_back(std::move(dest));
    }

    std::sort(rows.begin(), rows.end(),
              [](const Destination& a, const Destination& b) {
        if (a.unresolved != b.unresolved) {
            return !a.unresolved;
        }
        const int cmp = QString::localeAwareCompare(a.label, b.label);
        return cmp < 0 || (cmp == 0 && a.id < b.id);
    });

    for (const Destination& row : rows) {
        auto* item = new QListWidgetItem(row.label, destinations_);
        item->setData(Qt::UserRole, row.id);
        item->setData(Qt::UserRole + 1, row.search);
        if (!row.userId.isEmpty()) {
            item->setData(UserIdRole, row.userId);
            directRows_.insert(row.userId, item);
        }
    }
    if (destinations_->count()) {
        destinations_->setCurrentRow(0);
    }
}

void ForwardPostDialog::filterDestinations(const QString& text)
{
    // Stale remote search callbacks cannot inject results for an older query.
    ++searchGeneration_;
    searchTimer_.stop();
    removeRemoteResults();

    QListWidgetItem* firstVisible = nullptr;
    const QString term = text.trimmed();
    for (int i = 0; i < destinations_->count(); ++i) {
        QListWidgetItem* item = destinations_->item(i);
        const bool matches = item->data(Qt::UserRole + 1).toString()
            .contains(term, Qt::CaseInsensitive);
        item->setHidden(!matches);
        if (matches && !firstVisible) {
            firstVisible = item;
        }
    }
    if (!destinations_->currentItem()
        || destinations_->currentItem()->isHidden()) {
        destinations_->setCurrentItem(firstVisible);
    }
    if (!submitting_ && sourceType_ == BackendChannel::publicChannel
        && term.size() >= 2) {
        searchTimer_.start();
    }
    updateSubmitEnabled();
    resolveVisibleDirectNames();
}

void ForwardPostDialog::removeRemoteResults()
{
    // Remote candidates do not participate in the stable local destination
    // ordering. Deleting only these rows leaves selection/scroll unchanged
    // for every existing channel.
    for (QListWidgetItem* item : std::as_const(remoteRows_)) {
        const int row = destinations_->row(item);
        if (row >= 0) {
            delete destinations_->takeItem(row);
        }
    }
    remoteRows_.clear();
}

void ForwardPostDialog::resolveVisibleDirectNames()
{
    if (!destinations_ || inFlightProfiles_.size() >= MaxVisibleProfileRequests
        || destinations_->count() == 0) {
        return;
    }

    // Query the viewport indices rather than scanning every channel on every
    // scroll. Include two rows of overscan so names appear before entering.
    const QRect rect = destinations_->viewport()->rect();
    const QModelIndex first = destinations_->indexAt(QPoint(4, 2));
    const QModelIndex last = destinations_->indexAt(
        QPoint(4, qMax(2, rect.height() - 3)));
    if (!first.isValid() && !last.isValid()) {
        return;
    }
    const int firstRow = qMax(0, (first.isValid() ? first.row() : last.row()) - 2);
    const int lastRow = qMin(destinations_->count() - 1,
                             (last.isValid() ? last.row() : first.row()) + 2);
    for (int i = firstRow; i <= lastRow
             && inFlightProfiles_.size() < MaxVisibleProfileRequests; ++i) {
        const QListWidgetItem* item = destinations_->item(i);
        if (!item || item->isHidden() || item->data(RemoteUserRole).toBool()) {
            continue;
        }
        const QString userId = item->data(UserIdRole).toString();
        if (userId.isEmpty() || requestedProfiles_.contains(userId)
            || backend_.getStorage().getUserById(userId)) {
            continue;
        }
        requestedProfiles_.insert(userId);
        inFlightProfiles_.insert(userId);
        QPointer<ForwardPostDialog> guard(this);
        UserProfileService::instance(backend_).ensureUser(
            userId, [guard, userId](const BackendUser*) {
            if (!guard) {
                return;
            }
            guard->inFlightProfiles_.remove(userId);
            guard->updateResolvedUser(userId);
            guard->resolveVisibleDirectNames();
        });
    }
}

void ForwardPostDialog::updateResolvedUser(const QString& userId)
{
    auto it = directRows_.find(userId);
    if (it == directRows_.end()) {
        return;
    }
    const BackendUser* user = backend_.getStorage().getUserById(userId);
    QListWidgetItem* item = it.value();
    if (!user || !item) {
        return;
    }
    const QString channelId = item->data(Qt::UserRole).toString();
    const BackendChannel* channel =
        backend_.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    const QString label = labelForChannel(*channel, backend_.getStorage());
    const QString search = label + QLatin1Char(' ') + user->username;
    if (item->text() != label) {
        item->setText(label);
    }
    item->setData(Qt::UserRole + 1, search);
    const QString term = search_ ? search_->text().trimmed() : QString();
    if (!term.isEmpty()) {
        item->setHidden(!search.contains(term, Qt::CaseInsensitive));
    }
    // Deliberately never call sortItems(), rebuild the row model, or move the
    // current index: a delayed profile response must not move the click target.
    updateSubmitEnabled();
}

void ForwardPostDialog::searchRemoteUsers()
{
    if (!search_ || submitting_
        || sourceType_ != BackendChannel::publicChannel) {
        return;
    }
    const QString term = search_->text().trimmed();
    if (term.size() < 2) {
        return;
    }
    UserSearchOptions options;
    options.term = term;
    options.limit = MaxRemoteUserResults;
    const int generation = searchGeneration_;
    QPointer<ForwardPostDialog> guard(this);
    UserProfileService::instance(backend_).searchUsers(
        options, [guard, generation](QVector<const BackendUser*> users) {
        if (!guard || guard->submitting_
            || guard->searchGeneration_ != generation) {
            return;
        }
        guard->showRemoteUsers(users);
    });
}

void ForwardPostDialog::showRemoteUsers(
    const QVector<const BackendUser*>& users)
{
    removeRemoteResults();

    const Storage& storage = backend_.getStorage();
    for (const BackendUser* user : users) {
        if (!user || user->id.isEmpty() || user->delete_at
            || user->id == backend_.getLoginUser().id
            || storage.getDirectChannelByUserId(user->id)) {
            continue;
        }
        const QString name = user->getDisplayName().isEmpty()
            ? user->username : user->getDisplayName();
        const QString label = tr("%1 (@%2) · New direct message")
            .arg(name, user->username);
        auto* item = new QListWidgetItem(label, destinations_);
        item->setData(UserIdRole, user->id);
        item->setData(RemoteUserRole, true);
        item->setData(Qt::UserRole + 1, name + QLatin1Char(' ')
            + user->username);
        remoteRows_.push_back(item);
    }
    if (!destinations_->currentItem() || destinations_->currentItem()->isHidden()) {
        for (int i = 0; i < destinations_->count(); ++i) {
            QListWidgetItem* item = destinations_->item(i);
            if (!item->isHidden()) {
                destinations_->setCurrentItem(item);
                break;
            }
        }
    }
    updateSubmitEnabled();
}

void ForwardPostDialog::updateSubmitEnabled()
{
    const QListWidgetItem* selected = destinations_->currentItem();
    forwardButton_->setEnabled(
        !submitting_ && !sourcePostId_.isEmpty()
        && (!usePermalink_ || !sourceTeamSlug_.isEmpty())
        && selected && !selected->isHidden());
}

void ForwardPostDialog::showFailure(const QString& message)
{
    submitting_ = false;
    destinations_->setEnabled(true);
    if (search_) {
        search_->setEnabled(true);
    }
    comment_->setReadOnly(false);
    error_->setText(message.isEmpty()
        ? tr("The server could not forward this message.") : message);
    error_->show();
    updateSubmitEnabled();
}

void ForwardPostDialog::submit()
{
    if (submitting_ || sourcePostId_.isEmpty()
        || (usePermalink_ && sourceTeamSlug_.isEmpty())) {
        return;
    }
    const QListWidgetItem* selected = destinations_->currentItem();
    if (!selected || selected->isHidden()) {
        return;
    }
    const bool newDirect = selected->data(RemoteUserRole).toBool();
    const QString targetId = selected->data(Qt::UserRole).toString();
    const QString userId = selected->data(UserIdRole).toString();

    if (newDirect) {
        if (sourceType_ != BackendChannel::publicChannel || userId.isEmpty()) {
            return;
        }
    } else if (!canForwardPostTo(sourceType_, sourceChannelId_, targetId)) {
        return;
    }

    if (!usePermalink_) {
        if (sourceHasFileAttachments_) {
            showFailure(tr("This message has attachments. Standalone forwarding "
                           "requires copying files; enable permalink forwarding "
                           "in Settings to forward it."));
            return;
        }
        if (sourceMessage_.isEmpty() || sourceAuthorId_.isEmpty()) {
            showFailure(tr("The source message or original author is unavailable."));
            return;
        }
    }

    submitting_ = true;
    updateSubmitEnabled();
    destinations_->setEnabled(false);
    if (search_) {
        search_->setEnabled(false);
    }
    comment_->setReadOnly(true);
    error_->hide();

    if (!usePermalink_) {
        deliverToDestination(targetId, userId, newDirect);
        return;
    }

    QPointer<ForwardPostDialog> guard(this);
    PostPermalinkService::instance(backend_).resolve(
        sourcePostId_, sourceTeamSlug_, true,
        [guard, targetId, userId, newDirect](const QString& link,
                                              const QString& error) {
        if (!guard) return;
        if (link.isEmpty()) {
            guard->showFailure(error + ForwardPostDialog::tr(" No message was sent."));
            return;
        }
        guard->permalink_ = link;
        guard->deliverToDestination(targetId, userId, newDirect);
    });
}

void ForwardPostDialog::deliverToDestination(const QString& targetId,
                                             const QString& userId,
                                             bool newDirect)
{
    if (!newDirect) {
        sendToChannel(targetId);
        return;
    }

    // A user returned by server search may have no DM yet. Create/open it
    // before posting; leave the modal usable if the server rejects creation.
    if (BackendChannel* existing =
            backend_.getStorage().getDirectChannelByUserId(userId)) {
        sendToChannel(existing->id);
        return;
    }
    const BackendUser* targetUser = backend_.getStorage().getUserById(userId);
    if (!targetUser) {
        showFailure(tr("The selected user is no longer available."));
        return;
    }
    QPointer<ForwardPostDialog> guard(this);
    backend_.createDirectChannelDetailed(
        *targetUser,
        [guard](BackendChannel* channel, const QString& error) {
            if (!guard) {
                return;
            }
            if (channel) {
                guard->sendToChannel(channel->id);
            } else {
                guard->showFailure(error);
            }
        });
}

void ForwardPostDialog::sendToChannel(const QString& channelId)
{
    BackendChannel* target = backend_.getStorage().getChannelById(channelId);
    if (!target || !canForwardPostTo(sourceType_, sourceChannelId_, channelId)) {
        showFailure(tr("The destination conversation is no longer available."));
        return;
    }

    const QString message = usePermalink_
        ? forwardedPostText(comment_->toPlainText(), permalink_)
        : forwardedCopyText(comment_->toPlainText(), sourceMessage_);
    QJsonObject props;
    if (!usePermalink_) {
        QString teamId = backend_.getCurrentTeamContextId();
        if (teamId.isEmpty()) {
            teamId = target->team ? target->team->id : sourceTeamId_;
        }
        props = forwardedCopyProps(sourcePostId_, sourceAuthorId_, teamId);
    }
    const bool publicSource = sourceType_ == BackendChannel::publicChannel;
    QPointer<ForwardPostDialog> guard(this);
    PostCreateService::instance(backend_).createPostDetailed(
        *target, message, {}, {}, props, {},
        [guard, channelId, publicSource](PostCreateService::CreatePostResult result) {
        if (!guard) {
            return;
        }
        if (result.success()) {
            Backend& backend = guard->backend_;
            guard->accept();
            // Match Mattermost's navigation rule only after REST acknowledged.
            if (publicSource) {
                AppNavigationService::instance(backend).openChannel(channelId);
            }
            return;
        }
        guard->showFailure(
            result.errorText.isEmpty()
                ? ForwardPostDialog::tr("The server could not forward this message (HTTP %1).")
                      .arg(result.httpStatus)
                : result.errorText);
    });
}

} // namespace Mattermost
