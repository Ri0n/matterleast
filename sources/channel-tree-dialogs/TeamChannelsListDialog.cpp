/**
 * @file TeamChannelsListDialog.cpp
 * @brief Lazily loaded public-channel directory for one Mattermost team.
 */

#include "TeamChannelsListDialog.h"

#include <algorithm>
#include <utility>

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/PublicChannelPaging.h"
#include "backend/Storage.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree-dialogs/CreateChannelDialog.h"
#include "info-dialogs/ChannelInfoDialog.h"
#include "navigation/AppNavigationService.h"
#include "ui/AvatarUtils.h"
#include "ui_FilterListDialog.h"
#include "widgets/LongListWidget.h"

namespace Mattermost {
namespace {

constexpr int ChannelRowHeight = 54;
constexpr int ChannelIconExtent = 18;
constexpr int MetaIconExtent = 14;
constexpr int SearchDelayMs = 200;

class ElidedLabel final : public QLabel
{
public:
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(parent)
        , fullText_(text)
    {
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setToolTip(text);
        updateElision();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        updateElision();
    }

private:
    void updateElision()
    {
        const int availableWidth = std::max(1, width());
        QLabel::setText(fontMetrics().elidedText(
            fullText_, Qt::ElideRight, availableWidth));
    }

    QString fullText_;
};

class PublicChannelRow final : public QWidget
{
public:
    PublicChannelRow(BackendChannel& channel,
                     bool joined,
                     std::function<void(const QPoint&)> contextMenu,
                     std::function<void()> activated,
                     QWidget* parent = nullptr)
        : QWidget(parent)
        , contextMenu_(std::move(contextMenu))
        , activated_(std::move(activated))
    {
        // This is only LongListWidget's first estimate. The widget keeps its
        // natural dynamic size; LongListWidget measures the real sizeHint and
        // updates its height index exactly as it does for chat rows.
        setMinimumHeight(ChannelRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setCursor(Qt::PointingHandCursor);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 6, 12, 6);
        layout->setSpacing(3);

        auto* titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(8);

        auto* icon = new QLabel(this);
        icon->setFixedSize(ChannelIconExtent, ChannelIconExtent);
        icon->setAlignment(Qt::AlignCenter);
        icon->setPixmap(ChannelIcons::channel().pixmap(
            ChannelIconExtent, ChannelIconExtent));
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleRow->addWidget(icon, 0, Qt::AlignVCenter);

        auto* name = new ElidedLabel(channel.display_name, this);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleRow->addWidget(name, 1);
        layout->addLayout(titleRow);

        // Match Mattermost's Browse Channels information hierarchy: the second
        // line is membership/member-count/purpose metadata. Channel *header* is
        // intentionally not rendered here; it is often long rich content and
        // was what made the old directory look like a wall of text.
        auto* metaRow = new QHBoxLayout;
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(4);

        QFont metaFont = font();
        if (metaFont.pointSizeF() > 0.0) {
            metaFont.setPointSizeF(std::max(7.0, metaFont.pointSizeF() - 1.0));
        }

        auto makeMetaLabel = [this, &metaFont](const QString& text) {
            auto* label = new QLabel(text, this);
            label->setFont(metaFont);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            return label;
        };

        if (joined) {
            auto* membership = makeMetaLabel(tr("✓ Joined"));
            QPalette membershipPalette = membership->palette();
            membershipPalette.setColor(
                QPalette::WindowText, AvatarUtils::statusColor(QStringLiteral("online")));
            membership->setPalette(membershipPalette);
            metaRow->addWidget(membership);

            auto* dot = makeMetaLabel(QStringLiteral("·"));
            dot->setEnabled(false);
            metaRow->addWidget(dot);
        }

        auto* membersIcon = new QLabel(this);
        membersIcon->setFixedSize(MetaIconExtent, MetaIconExtent);
        membersIcon->setPixmap(ChannelIcons::member().pixmap(
            QSize(MetaIconExtent, MetaIconExtent), QIcon::Disabled));
        membersIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
        metaRow->addWidget(membersIcon, 0, Qt::AlignVCenter);

        auto* memberCount = makeMetaLabel(
            channel.member_count >= 0
                ? QString::number(channel.member_count)
                : QStringLiteral("…"));
        memberCount->setEnabled(false);
        metaRow->addWidget(memberCount);

        const QString purpose = channel.purpose.trimmed();
        if (!purpose.isEmpty()) {
            auto* dot = makeMetaLabel(QStringLiteral("·"));
            dot->setEnabled(false);
            metaRow->addWidget(dot);

            auto* purposeLabel = new ElidedLabel(purpose, this);
            purposeLabel->setFont(metaFont);
            purposeLabel->setEnabled(false);
            purposeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            metaRow->addWidget(purposeLabel, 1);
        } else {
            metaRow->addStretch(1);
        }

        layout->addLayout(metaRow);

        connect(this, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) {
            if (contextMenu_) {
                contextMenu_(mapToGlobal(pos));
            }
        });
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event && event->button() == Qt::LeftButton && activated_) {
            activated_();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        QColor separator = palette().color(QPalette::Mid);
        separator.setAlpha(70);
        painter.fillRect(0, height() - 1, width(), 1, separator);
    }

private:
    std::function<void(const QPoint&)> contextMenu_;
    std::function<void()> activated_;
};

class PublicChannelLongList final : public LongListWidget
{
public:
    using Factory = std::function<QWidget*(int)>;

    PublicChannelLongList(Factory factory, QWidget* parent)
        : LongListWidget(parent)
        , factory_(std::move(factory))
    {
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return factory_ ? factory_(index) : nullptr;
    }

private:
    Factory factory_;
};

} // namespace

TeamChannelsListDialog::TeamChannelsListDialog(
    Backend& backend,
    const FilterListDialogConfig& cfg,
    BackendTeam& team,
    QWidget* parent)
    : FilterListDialog(parent)
    , backend(backend)
    , team(team)
{
    setupVirtualList(cfg);
    pagedItemCount = PublicChannelsPerPage;
    pageChannels.resize(pagedItemCount);
    channelList->setItemCount(pagedItemCount);
    ui->usersCountLabel->setText(tr("Loading channels…"));
}

TeamChannelsListDialog::~TeamChannelsListDialog()
{
    finishAllRequests();
}

void TeamChannelsListDialog::showForTeam(Backend& backend,
                                            BackendTeam& team,
                                            QWidget* parent)
{
    FilterListDialogConfig dialogCfg {
        tr("Public Channels - Mattermost"),
        tr("Public Channels in team '%1':").arg(team.display_name),
        tr("Filter channels by name:"),
        QDialogButtonBox::Close,
        QString()
    };

    auto* dialog = new TeamChannelsListDialog(backend, dialogCfg, team, parent);
    dialog->show();
}

void TeamChannelsListDialog::setupVirtualList(const FilterListDialogConfig& cfg)
{
    FilterListDialog::create(cfg);

    auto* createButton = ui->buttonBox->addButton(
        tr("Create channel…"), QDialogButtonBox::ActionRole);
    createButton->setToolTip(tr("Create a public or private channel"));
    connect(createButton, &QPushButton::clicked,
            this, &TeamChannelsListDialog::openCreateChannelDialog);

    ui->tableWidget->hide();
    const int tableIndex = ui->verticalLayout->indexOf(ui->tableWidget);

    channelList = new PublicChannelLongList(
        [this](int index) { return createChannelRow(index); }, this);
    channelList->setDefaultItemHeight(ChannelRowHeight);
    channelList->setMaterializationLimit(120);
    channelList->setRequestBlockSize(32);
    channelList->setPrefetchScreens(1);
    ui->verticalLayout->insertWidget(std::max(0, tableIndex), channelList, 1);

    connect(channelList, &LongListWidget::rangeRequested, this,
            [this](int first, int last, LongListWidget::RequestReason, quint64) {
        requestChannelRange(first, last);
    });

    // FilterListDialog's legacy handler only knows about its hidden QTableWidget.
    // Replace it with server-backed search so unloaded channels remain searchable.
    QObject::disconnect(ui->filterLineEdit, nullptr, this, nullptr);
    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(SearchDelayMs);
    connect(searchTimer, &QTimer::timeout, this, &TeamChannelsListDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited, this,
            [this](const QString& text) { filterEdited(text); });
}

QWidget* TeamChannelsListDialog::createChannelRow(int index)
{
    BackendChannel* channel = channelAt(index);
    if (!channel) {
        return nullptr;
    }
    return new PublicChannelRow(
        *channel,
        isJoinedChannel(*channel),
        [this, channel](const QPoint& globalPos) {
            showChannelContextMenu(channel, globalPos);
        },
        [this, channel] {
            activateChannel(channel);
        },
        channelList);
}

BackendChannel* TeamChannelsListDialog::channelAt(int index) const
{
    const QVector<BackendChannel*>& channels = searchMode ? searchChannels : pageChannels;
    return index >= 0 && index < channels.size() ? channels.at(index) : nullptr;
}

bool TeamChannelsListDialog::isJoinedChannel(const BackendChannel& channel) const
{
    return backend.getStorage().getChannelById(channel.id) != nullptr;
}

void TeamChannelsListDialog::activateChannel(BackendChannel* channel)
{
    if (!channel) {
        return;
    }

    if (isJoinedChannel(*channel)) {
        AppNavigationService::instance(backend).openChannel(channel->id);
        accept();
        return;
    }

    const QString channelId = channel->id;
    QPointer<TeamChannelsListDialog> guard(this);
    backend.joinChannel(*channel, [guard, channelId] {
        if (!guard) {
            return;
        }
        guard->backend.retrieveChannel(
            guard->team, channelId, [guard, channelId](BackendChannel&) {
                if (!guard) {
                    return;
                }
                AppNavigationService::instance(guard->backend).openChannel(channelId);
                guard->accept();
            });
    });
}

void TeamChannelsListDialog::openCreateChannelDialog()
{
    auto* dialog = new CreateChannelDialog(team, this);
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
        const QString name = dialog->channelName();
        const QString displayName = dialog->displayName();
        const QString purpose = dialog->purpose();
        const bool privateChannel = dialog->isPrivateChannel();

        QPointer<TeamChannelsListDialog> guard(this);
        backend.createChannel(
            team, name, displayName, purpose, privateChannel,
            [guard](BackendChannel& channel) {
                if (!guard) {
                    return;
                }
                AppNavigationService::instance(guard->backend).openChannel(channel.id);
                guard->accept();
            });
    });
    dialog->show();
}

void TeamChannelsListDialog::requestMemberCounts(
    const QVector<BackendChannel*>& channels)
{
    QStringList ids;
    ids.reserve(channels.size());
    for (BackendChannel* channel : channels) {
        if (channel && !channel->id.isEmpty()) {
            ids.push_back(channel->id);
        }
    }
    ids.removeDuplicates();
    if (ids.isEmpty()) {
        return;
    }

    QPointer<TeamChannelsListDialog> guard(this);
    backend.retrieveChannelsMemberCounts(
        ids, [guard](QJsonObject counts) {
            if (guard) {
                guard->applyMemberCounts(counts);
            }
        });
}

void TeamChannelsListDialog::applyMemberCounts(const QJsonObject& counts)
{
    if (counts.isEmpty()) {
        return;
    }

    auto update = [&counts](QVector<BackendChannel*>& channels) {
        for (BackendChannel* channel : channels) {
            if (!channel) {
                continue;
            }
            const auto it = counts.constFind(channel->id);
            if (it != counts.constEnd()) {
                channel->member_count = it->toInt(-1);
            }
        }
    };
    update(pageChannels);
    update(searchChannels);

    if (!channelList) {
        return;
    }
    for (int index : channelList->materializedIndices()) {
        BackendChannel* channel = channelAt(index);
        if (channel && counts.contains(channel->id)) {
            channelList->replaceItem(index);
        }
    }
}

void TeamChannelsListDialog::showChannelContextMenu(BackendChannel* channel,
                                                     const QPoint& globalPos)
{
    if (!channel) {
        return;
    }
    QMenu menu(this);
    menu.addAction(isJoinedChannel(*channel) ? tr("Open channel")
                                             : tr("Join this channel"),
                   this, [this, channel] { activateChannel(channel); });
    menu.addAction(tr("View channel details"), this, [this, channel] {
        auto* dialog = new ChannelInfoDialog(*channel, this);
        dialog->show();
    });
    menu.exec(globalPos);
}

void TeamChannelsListDialog::addContextMenuActions(QMenu& menu,
                                                    const QVariant& selectedItemData)
{
    BackendChannel* channel = selectedItemData.value<BackendChannel*>();
    if (!channel) {
        return;
    }
    menu.addAction(isJoinedChannel(*channel) ? tr("Open channel")
                                             : tr("Join this channel"),
                   this, [this, channel] { activateChannel(channel); });
    menu.addAction(tr("View channel details"), this, [this, channel] {
        auto* dialog = new ChannelInfoDialog(*channel, this);
        dialog->show();
    });
}

void TeamChannelsListDialog::setItemCountLabel(uint32_t count)
{
    ui->usersCountLabel->setText(QString::number(count)
        + (count == 1 ? tr(" channel") : tr(" channels")));
}

void TeamChannelsListDialog::updatePagedCountLabel(bool hasMore)
{
    ui->usersCountLabel->setText(QString::number(concretePagedCount)
        + (hasMore ? QStringLiteral("+ channels")
                   : (concretePagedCount == 1 ? tr(" channel") : tr(" channels"))));
}

void TeamChannelsListDialog::requestChannelRange(int first, int last)
{
    if (!channelList || first < 0 || last < first) {
        return;
    }
    if (searchMode) {
        channelList->finishRangeRequest(first, last);
        return;
    }

    PendingRange pending;
    pending.first = first;
    pending.last = last;
    const int firstPage = first / PublicChannelsPerPage;
    const int lastPage = last / PublicChannelsPerPage;
    for (int page = firstPage; page <= lastPage; ++page) {
        if (!loadedPages.contains(page)) {
            pending.pages.insert(page);
            loadPage(page);
        }
    }

    if (pending.pages.isEmpty()) {
        channelList->finishRangeRequest(first, last);
    } else {
        pendingRanges.push_back(std::move(pending));
    }
}

void TeamChannelsListDialog::loadPage(int page)
{
    if (page < 0 || loadedPages.contains(page) || inFlightPages.contains(page)) {
        return;
    }
    inFlightPages.insert(page);
    QPointer<TeamChannelsListDialog> guard(this);
    backend.retrieveTeamPublicChannelsPage(
        team.id, page, PublicChannelsPerPage,
        [guard, page](QJsonArray values) {
            if (!guard) {
                return;
            }

            guard->inFlightPages.remove(page);
            const int pageStart = page * PublicChannelsPerPage;
            if (guard->pageChannels.size() < pageStart + values.size()) {
                guard->pageChannels.resize(pageStart + values.size());
            }

            int offset = 0;
            QVector<BackendChannel*> countTargets;
            countTargets.reserve(values.size());
            for (const auto& value : values) {
                const QJsonObject object = value.toObject();
                if (BackendChannel::getChannelType(object) != BackendChannel::publicChannel) {
                    continue;
                }
                guard->pageStorage.emplace_back(guard->backend.getStorage(), object);
                BackendChannel* channel = &guard->pageStorage.back();
                guard->pageChannels[pageStart + offset] = channel;
                countTargets.push_back(channel);
                ++offset;
            }

            // The public-channel endpoint itself only returns open channels, so
            // filtering here is defensive. Treat any omitted non-open row as a
            // short page rather than inventing unavailable logical slots.
            const int returnedCount = offset;
            guard->loadedPages.insert(page);
            guard->concretePagedCount = std::max(
                guard->concretePagedCount, pageStart + returnedCount);
            guard->pagedEndKnown = publicChannelPageProvesEnd(returnedCount);
            guard->pagedItemCount = publicChannelLogicalCountAfterPage(page, returnedCount);

            if (!guard->searchMode) {
                guard->channelList->setItemCount(guard->pagedItemCount);
                if (returnedCount > 0) {
                    guard->channelList->setRangeAvailable(
                        pageStart, pageStart + returnedCount - 1);
                }
                guard->updatePagedCountLabel(!guard->pagedEndKnown);
            }
            guard->requestMemberCounts(countTargets);
            guard->completePage(page);
        });
}

void TeamChannelsListDialog::completePage(int page)
{
    int index = 0;
    while (index < pendingRanges.size()) {
        PendingRange& pending = pendingRanges[index];
        pending.pages.remove(page);
        if (!pending.pages.isEmpty()) {
            ++index;
            continue;
        }
        if (channelList) {
            channelList->finishRangeRequest(pending.first, pending.last);
        }
        pendingRanges.removeAt(index);
    }
}

void TeamChannelsListDialog::finishAllRequests()
{
    if (channelList) {
        for (const PendingRange& pending : std::as_const(pendingRanges)) {
            channelList->finishRangeRequest(pending.first, pending.last);
        }
    }
    pendingRanges.clear();
}

void TeamChannelsListDialog::reapplyLoadedPages()
{
    if (!channelList || searchMode) {
        return;
    }
    for (int page : std::as_const(loadedPages)) {
        const int first = page * PublicChannelsPerPage;
        const int last = std::min(first + PublicChannelsPerPage,
                                  static_cast<int>(pageChannels.size())) - 1;
        int concreteLast = last;
        while (concreteLast >= first && !pageChannels.value(concreteLast)) {
            --concreteLast;
        }
        if (concreteLast >= first) {
            channelList->setRangeAvailable(first, concreteLast);
        }
    }
}

void TeamChannelsListDialog::filterEdited(const QString& text)
{
    searchTerm = text.trimmed();
    ++searchGeneration;
    searchTimer->stop();

    if (searchTerm.isEmpty()) {
        restorePagedMode();
        return;
    }

    finishAllRequests();
    searchMode = true;
    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    ui->usersCountLabel->setText(tr("Searching…"));
    searchTimer->start();
}

void TeamChannelsListDialog::performSearch()
{
    if (searchTerm.isEmpty()) {
        return;
    }
    const int generation = searchGeneration;
    const QString term = searchTerm;
    QPointer<TeamChannelsListDialog> guard(this);
    backend.searchTeamPublicChannels(team.id, term,
        [guard, generation](QJsonArray results) {
            if (guard) {
                guard->enterSearchResults(std::move(results), generation);
            }
        });
}

void TeamChannelsListDialog::enterSearchResults(QJsonArray results, int generation)
{
    if (generation != searchGeneration || searchTerm.isEmpty()) {
        return;
    }

    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    for (const auto& value : results) {
        const QJsonObject object = value.toObject();
        if (BackendChannel::getChannelType(object) != BackendChannel::publicChannel) {
            continue;
        }
        searchStorage.emplace_back(backend.getStorage(), object);
        searchChannels.push_back(&searchStorage.back());
    }

    requestMemberCounts(searchChannels);

    channelList->setItemCount(searchChannels.size());
    if (!searchChannels.isEmpty()) {
        channelList->setRangeAvailable(0, searchChannels.size() - 1);
    }
    setItemCountLabel(static_cast<uint32_t>(searchChannels.size()));
}

void TeamChannelsListDialog::restorePagedMode()
{
    finishAllRequests();
    searchMode = false;
    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    channelList->setItemCount(pagedItemCount);
    reapplyLoadedPages();
    updatePagedCountLabel(!pagedEndKnown);
}

} // namespace Mattermost
