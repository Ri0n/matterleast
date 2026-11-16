#include "ForwardPostDialog.h"

#include <algorithm>
#include <utility>

#include <QAbstractItemView>
#include <QColor>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVector>
#include <QVBoxLayout>

#include "PostForwardPolicy.h"
#include "PostPermalinkUtils.h"
#include "backend/Backend.h"
#include "backend/PostCreateService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/QuotedPostPreview.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

struct Destination {
    QString id;
    QString label;
    QString search;
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

} // namespace

ForwardPostDialog::ForwardPostDialog(
    Backend& backend, const BackendPost& post, QWidget* parent)
    : QDialog(parent)
    , backend_(backend)
    , sourceChannelId_(post.channel_id)
    , permalink_(messagePermalink(post))
{
    setObjectName(QStringLiteral("forwardPostDialog"));
    setWindowTitle(tr("Forward message"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(540, 520);

    const BackendChannel* source =
        backend_.getStorage().getChannelById(sourceChannelId_);
    sourceType_ = source ? source->type : BackendChannel::unknown;

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

    populateDestinations();
    updateSubmitEnabled();
    if (search_) {
        search_->setFocus();
    }
}

void ForwardPostDialog::populateDestinations()
{
    // The local channel directory contains established/joined conversations.
    // Never scan channel histories or introduce private channels from search
    // results: the original client's privacy restriction is enforced below.
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
        dest.label = channelLabel(*channel);
        dest.search = dest.label + QLatin1Char(' ') + channel->name;
        rows.push_back(std::move(dest));
    }
    std::sort(rows.begin(), rows.end(),
              [](const Destination& a, const Destination& b) {
        const int cmp = QString::localeAwareCompare(a.label, b.label);
        return cmp < 0 || (cmp == 0 && a.id < b.id);
    });

    for (const Destination& row : rows) {
        auto* item = new QListWidgetItem(row.label, destinations_);
        item->setData(Qt::UserRole, row.id);
        item->setData(Qt::UserRole + 1, row.search);
    }
    if (destinations_->count()) {
        destinations_->setCurrentRow(0);
    }
}

void ForwardPostDialog::filterDestinations(const QString& text)
{
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
    updateSubmitEnabled();
}

void ForwardPostDialog::updateSubmitEnabled()
{
    const QListWidgetItem* selected = destinations_->currentItem();
    forwardButton_->setEnabled(
        !submitting_ && !permalink_.isEmpty()
        && selected && !selected->isHidden());
}

void ForwardPostDialog::submit()
{
    if (submitting_ || permalink_.isEmpty()) {
        return;
    }
    const QListWidgetItem* selected = destinations_->currentItem();
    if (!selected || selected->isHidden()) {
        return;
    }
    const QString targetId = selected->data(Qt::UserRole).toString();
    if (!canForwardPostTo(sourceType_, sourceChannelId_, targetId)) {
        return;
    }
    BackendChannel* target = backend_.getStorage().getChannelById(targetId);
    if (!target) {
        error_->setText(tr("The destination conversation is no longer available."));
        error_->show();
        return;
    }

    const QString message = forwardedPostText(
        comment_->toPlainText(), permalink_);
    submitting_ = true;
    updateSubmitEnabled();
    destinations_->setEnabled(false);
    if (search_) search_->setEnabled(false);
    comment_->setReadOnly(true);
    error_->hide();

    const bool publicSource = sourceType_ == BackendChannel::publicChannel;
    QPointer<ForwardPostDialog> guard(this);
    PostCreateService::instance(backend_).createPostDetailed(
        *target, message, {}, {}, {}, {},
        [guard, targetId, publicSource](PostCreateService::CreatePostResult result) {
        if (!guard) {
            return;
        }
        if (result.success()) {
            Backend& backend = guard->backend_;
            guard->accept();
            // The official client navigates to the destination for public
            // forwards, but keeps the original view for private re-shares.
            if (publicSource) {
                AppNavigationService::instance(backend).openChannel(targetId);
            }
            return;
        }
        guard->submitting_ = false;
        guard->destinations_->setEnabled(true);
        if (guard->search_) guard->search_->setEnabled(true);
        guard->comment_->setReadOnly(false);
        guard->error_->setText(
            result.errorText.isEmpty()
                ? ForwardPostDialog::tr("The server could not forward this message (HTTP %1).")
                      .arg(result.httpStatus)
                : result.errorText);
        guard->error_->show();
        guard->updateSubmitEnabled();
    });
}

} // namespace Mattermost
