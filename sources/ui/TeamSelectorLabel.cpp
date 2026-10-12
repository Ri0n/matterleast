#include "TeamSelectorLabel.h"

#include <algorithm>
#include <vector>

#include <QAbstractItemModel>
#include <QAction>
#include <QFrame>
#include <QEvent>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPainter>
#include <QPaintEvent>
#include <QStyleOptionToolButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree-dialogs/TeamChannelsListDialog.h"
#include "options/MLOptions.h"
#include "ui/IconUtils.h"

namespace Mattermost {
namespace {

constexpr char ActiveTeamSetting[] = "sidebar/active_team_id";

QString teamLabel(const BackendTeam& team)
{
    if (!team.display_name.trimmed().isEmpty()) {
        return team.display_name.trimmed();
    }
    if (!team.name.trimmed().isEmpty()) {
        return team.name.trimmed();
    }
    return team.id;
}

QString joinableTeamLabel(const QJsonObject& team)
{
    QString display = team.value(QStringLiteral("display_name")).toString().trimmed();
    const QString name = team.value(QStringLiteral("name")).toString().trimmed();
    if (display.isEmpty()) {
        display = name;
    }
    if (display.isEmpty()) {
        display = team.value(QStringLiteral("id")).toString();
    }
    if (!name.isEmpty() && name.compare(display, Qt::CaseInsensitive) != 0) {
        display += QStringLiteral(" (") + name + QLatin1Char(')');
    }
    return display;
}

} // namespace

TeamSelectorLabel::TeamSelectorLabel(QWidget* parent)
    : QToolButton(parent)
    , preferredTeamId_(MLOptions::instance()->value<QString>(
          QString::fromLatin1(ActiveTeamSetting)))
{
    setMinimumHeight(18);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setAutoRaise(true);
    setAccessibleName(tr("Current team"));
    setCursor(Qt::PointingHandCursor);

    QFont labelFont = font();
    labelFont.setBold(true);
    setFont(labelFont);

    // The hidden selector is only the team context controller.
}

bool TeamSelectorLabel::activateTeam(QWidget* context, const QString& teamId)
{
    if (!context || teamId.isEmpty()) {
        return false;
    }
    QWidget* root = context->window();
    auto* selector = root
        ? root->findChild<TeamSelectorLabel*>(QStringLiteral("teamSelectorLabel"))
        : nullptr;
    return selector && selector->setActiveTeam(teamId, true);
}



namespace {
class TeamPillButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isChecked() || underMouse()) {
            QColor fill = palette().color(QPalette::Text);
            fill.setAlpha(isChecked() ? 36 : 14);
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1), 6, 6);
            p.fillPath(path, fill);
            if (isChecked()) {
                QColor edge = palette().color(QPalette::Text);
                edge.setAlpha(85);
                p.setPen(QPen(edge, 1));
                p.drawPath(path);
            }
        }
        QFont f = font();
        f.setBold(isChecked());
        p.setFont(f);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect().adjusted(9, 0, -8, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};
class TeamDivider final : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QColor gray = palette().color(QPalette::Text);
        gray.setAlpha(90);
        p.setPen(QPen(gray, 1, Qt::DotLine));
        p.drawLine(width()/2, 6, width()/2, height()-7);
    }
};
} // namespace

QWidget* TeamSelectorLabel::createTeamToolbar(QWidget* parent)
{
    if (_teamToolbar) {
        return _teamToolbar;
    }
    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("teamToolbar"));
    bar->setFixedHeight(34);
    _teamToolbar = bar;
    auto* outer = new QHBoxLayout(bar);
    outer->setContentsMargins(4, 2, 4, 2);
    outer->setSpacing(3);

    auto* scroll = new QScrollArea(bar);
    scroll->setObjectName(QStringLiteral("teamButtonScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scroll->setFixedHeight(30);
    scroll->viewport()->setAutoFillBackground(false);

    auto* previous = new QToolButton(bar);
    previous->setObjectName(QStringLiteral("teamToolbarPrevious"));
    previous->setText(QStringLiteral("‹"));
    previous->setToolTip(tr("Previous teams"));
    previous->setAutoRaise(true);
    previous->setFixedSize(17, 26);
    previous->hide();
    outer->addWidget(previous);

    _teamButtonsHost = new QWidget(scroll);
    _teamButtonsLayout = new QHBoxLayout(_teamButtonsHost);
    _teamButtonsLayout->setContentsMargins(0, 0, 0, 0);
    _teamButtonsLayout->setSpacing(3);
    // Qt may otherwise distribute spare width around the lone team pill.
    // Keep the team row at the same left edge as the channel list.
    _teamButtonsLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    scroll->setWidget(_teamButtonsHost);
    outer->addWidget(scroll, 1);

    auto* next = new QToolButton(bar);
    next->setObjectName(QStringLiteral("teamToolbarNext"));
    next->setText(QStringLiteral("›"));
    next->setToolTip(tr("More teams"));
    next->setAutoRaise(true);
    next->setFixedSize(17, 26);
    next->hide();
    outer->addWidget(next);

    QScrollBar* horizontalBar = scroll->horizontalScrollBar();
    connect(previous, &QToolButton::clicked, horizontalBar,
            [horizontalBar] {
        horizontalBar->setValue(horizontalBar->value() - 90);
    });
    connect(next, &QToolButton::clicked, horizontalBar,
            [horizontalBar] {
        horizontalBar->setValue(horizontalBar->value() + 90);
    });
    auto updateArrows = [horizontalBar, previous, next] {
        const bool overflow = horizontalBar->maximum() > horizontalBar->minimum();
        previous->setVisible(overflow);
        next->setVisible(overflow);
        previous->setEnabled(horizontalBar->value() > horizontalBar->minimum());
        next->setEnabled(horizontalBar->value() < horizontalBar->maximum());
    };
    connect(horizontalBar, &QScrollBar::rangeChanged, bar,
            [updateArrows](int, int) { updateArrows(); });
    connect(horizontalBar, &QScrollBar::valueChanged, bar,
            [updateArrows](int) { updateArrows(); });

    auto* add = new QToolButton(bar);
    add->setObjectName(QStringLiteral("teamToolbarAdd"));
    add->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/add"), palette().color(QPalette::Text)));
    add->setIconSize(QSize(20, 20));
    add->setAutoRaise(true);
    add->setFixedSize(30, 28);
    add->setToolTip(tr("Add another team"));
    add->setAccessibleName(tr("Add another team"));
    connect(add, &QToolButton::clicked, this, [this] { addAnotherTeam(); });
    outer->addWidget(add);

    auto* browse = new QToolButton(bar);
    browse->setObjectName(QStringLiteral("teamToolbarBrowse"));
    browse->setIcon(ChannelIcons::channel());
    browse->setIconSize(QSize(20, 20));
    browse->setAutoRaise(true);
    browse->setFixedSize(28, 28);
    browse->setToolTip(tr("Browse public channels"));
    browse->setAccessibleName(tr("Browse public channels"));
    connect(browse, &QToolButton::clicked, this, [this] { showPublicChannels(); });
    outer->insertWidget(outer->count() - 1, browse);

    // The legacy header selector is hidden, so its showEvent cannot attach.
    attachTree();
    rebuildTeamToolbar();
    return bar;
}

void TeamSelectorLabel::rebuildTeamToolbar()
{
    if (!_teamButtonsHost || !_teamButtonsLayout) return;

    std::vector<BackendTeam*> teams;
    if (tree_ && tree_->backendInstance()) {
        for (auto& entry : tree_->backendInstance()->getStorage().teams) {
            if (hasTeamRoot(entry.first)) teams.push_back(&entry.second);
        }
        std::sort(teams.begin(), teams.end(),
                  [](const BackendTeam* a, const BackendTeam* b) {
            return teamLabel(*a).compare(teamLabel(*b), Qt::CaseInsensitive) < 0;
        });
    }

    // Model rowsInserted can schedule several refreshes for a single team
    // admission. Preserve live buttons whenever membership/order is unchanged:
    // pending callbacks must never invalidate a button awaiting a mouse click.
    QStringList existingIds, desiredIds;
    for (int i = 0; i < _teamButtonsLayout->count(); ++i) {
        QWidget* widget = _teamButtonsLayout->itemAt(i)->widget();
        if (widget && !widget->property("teamId").toString().isEmpty()) {
            existingIds.push_back(widget->property("teamId").toString());
        }
    }
    for (const BackendTeam* team : teams) desiredIds.push_back(team->id);
    if (existingIds == desiredIds) {
        int minimumWidth = 0;
        for (int i = 0; i < _teamButtonsLayout->count(); ++i) {
            QWidget* widget = _teamButtonsLayout->itemAt(i)->widget();
            if (!widget) continue;
            minimumWidth += widget->width() + _teamButtonsLayout->spacing();
            if (auto* button = qobject_cast<QToolButton*>(widget)) {
                const QString id = button->property("teamId").toString();
                for (const BackendTeam* team : teams) {
                    if (team->id != id) continue;
                    const QString label = teamLabel(*team);
                    const QString title = button->fontMetrics().elidedText(
                        label, Qt::ElideRight, 175);
                    button->setText(title);
                    button->setToolTip(label);
                    button->setAccessibleName(label);
                    button->setFixedWidth(std::max(
                        40, button->fontMetrics().horizontalAdvance(title) + 20));
                    break;
                }
            }
        }
        _teamButtonsHost->setMinimumWidth(std::max(1, minimumWidth));
        updateTeamToolbarSelection();
        return;
    }

    // Roots were genuinely added/removed. Retire old widgets after the current
    // event turn; ordinary selection never comes through this branch.
    while (QLayoutItem* item = _teamButtonsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    int totalWidth = 0;
    for (BackendTeam* team : teams) {
        if (totalWidth) {
            auto* divider = new TeamDivider(_teamButtonsHost);
            divider->setObjectName(QStringLiteral("teamToolbarSeparator"));
            divider->setFixedSize(9, 28);
            _teamButtonsLayout->addWidget(divider);
            totalWidth += 9 + _teamButtonsLayout->spacing();
        }
        const QString id = team->id;
        const QString name = teamLabel(*team);
        auto* button = new TeamPillButton(_teamButtonsHost);
        button->setObjectName(QStringLiteral("teamToolbarTeamButton"));
        button->setProperty("teamId", id);
        const QString title = button->fontMetrics().elidedText(
            name, Qt::ElideRight, 175);
        button->setText(title);
        button->setToolTip(name);
        button->setAccessibleName(name);
        button->setCheckable(true);
        button->setChecked(id == activeTeamId_);
        button->setFixedSize(
            std::max(40, button->fontMetrics().horizontalAdvance(title) + 20), 28);
        connect(button, &QToolButton::clicked, this,
                [this, id] { setActiveTeam(id, true); });
        _teamButtonsLayout->addWidget(button);
        totalWidth += button->width() + _teamButtonsLayout->spacing();
        if (id == activeTeamId_) {
            // Rebuilding after a team switch must reveal the selected
            // button, even if the row extends beyond the viewport.
            QPointer<QToolButton> selected(button);
            QPointer<QScrollArea> scroll = _teamToolbar->findChild<QScrollArea*>(
                QStringLiteral("teamButtonScrollArea"));
            if (scroll) {
                QTimer::singleShot(0, scroll, [scroll, selected] {
                    if (scroll && selected) {
                        scroll->ensureWidgetVisible(selected);
                    }
                });
            }
        }
    }
    _teamButtonsHost->setMinimumWidth(std::max(1, totalWidth));
    _teamButtonsHost->adjustSize();
}

void TeamSelectorLabel::updateTeamToolbarSelection()
{
    if (!_teamButtonsHost) return;
    QToolButton* selected = nullptr;
    for (auto* button : _teamButtonsHost->findChildren<QToolButton*>(
             QStringLiteral("teamToolbarTeamButton"))) {
        const bool active = button->property("teamId").toString() == activeTeamId_;
        if (button->isChecked() != active) {
            button->setChecked(active);
        }
        button->update();
        if (active) selected = button;
    }
    if (!selected || !_teamToolbar) return;
    if (auto* scroll = _teamToolbar->findChild<QScrollArea*>(
            QStringLiteral("teamButtonScrollArea"))) {
        QPointer<QToolButton> safeButton(selected);
        QPointer<QScrollArea> safeScroll(scroll);
        QTimer::singleShot(0, scroll, [safeButton, safeScroll] {
            if (safeButton && safeScroll) safeScroll->ensureWidgetVisible(safeButton);
        });
    }
}

void TeamSelectorLabel::showEvent(QShowEvent* event)
{
    QToolButton::showEvent(event);
    attachTree();
}

void TeamSelectorLabel::changeEvent(QEvent* event)
{
    QToolButton::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        // The toolbar is detached from this hidden controller's own layout.
        // Rebuild after Qt finishes propagating the new application palette.
        QTimer::singleShot(0, this, [this] { rebuildTeamToolbar(); });
    }
}

void TeamSelectorLabel::paintEvent(QPaintEvent* event)
{
    QToolButton::paintEvent(event);

    QStyleOptionToolButton option;
    initStyleOption(&option);
    const QRect textRect = style()->subControlRect(
        QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, this);
    const QRect actualTextRect = fontMetrics().boundingRect(
        textRect, Qt::AlignCenter | Qt::TextShowMnemonic, text());
    if (actualTextRect.isEmpty()) {
        return;
    }

    const int x = actualTextRect.left();
    const int y = actualTextRect.bottom() + 1;
    QPen pen(palette().color(QPalette::ButtonText));
    pen.setStyle(Qt::DotLine);
    pen.setWidthF(1.0);

    QPainter painter(this);
    painter.setOpacity(underMouse() ? 0.8 : 0.45);
    painter.setPen(pen);
    painter.drawLine(x, y, actualTextRect.right(), y);
}

void TeamSelectorLabel::attachTree()
{
    auto* tree = window()
        ? window()->findChild<ChannelTree*>(QStringLiteral("channelList"))
        : nullptr;
    if (!tree) {
        QTimer::singleShot(0, this, [this] { attachTree(); });
        return;
    }
    if (tree_ == tree) {
        refreshTeams();
        return;
    }

    tree_ = tree;

    auto refreshLater = [this] {
        QTimer::singleShot(0, this, [this] {
            refreshTeams();
            rebuildTeamToolbar();
        });
    };
    connect(tree_->model(), &QAbstractItemModel::rowsInserted,
            this, [refreshLater] { refreshLater(); });
    connect(tree_->model(), &QAbstractItemModel::rowsRemoved,
            this, [refreshLater] { refreshLater(); });
    connect(tree_->model(), &QAbstractItemModel::modelReset,
            this, [refreshLater] { refreshLater(); });
    connect(tree_, &QObject::destroyed, this, [this] { tree_ = nullptr; });

    refreshTeams();
}

bool TeamSelectorLabel::hasTeamRoot(const QString& teamId) const
{
    if (!tree_ || teamId.isEmpty()) {
        return false;
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item
            && item->data(0, ChannelTree::ItemKindRole).toInt()
                == ChannelTree::TeamItemKind
            && item->data(0, ChannelTree::ItemTeamIdRole).toString() == teamId) {
            return true;
        }
    }
    return false;
}

void TeamSelectorLabel::refreshTeams()
{
    if (!tree_) {
        return;
    }
    Backend* backend = tree_->backendInstance();
    if (!backend) {
        setText(tr("Team"));
        return;
    }

    QString firstRootId;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item
            || item->data(0, ChannelTree::ItemKindRole).toInt()
                != ChannelTree::TeamItemKind) {
            continue;
        }
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        // Older selector code hid inactive top-level roots. rootIndex now scopes
        // the view instead, so every logical team root must remain addressable.
        item->setHidden(false);
        if (firstRootId.isEmpty()) {
            firstRootId = item->data(0, ChannelTree::ItemTeamIdRole).toString();
        }
    }

    if (firstRootId.isEmpty()) {
        tree_->setRootIndex(QModelIndex());
        setText(tr("Team"));
        rebuildTeamToolbar();
        return;
    }

    if (!pendingJoinedTeamId_.isEmpty() && hasTeamRoot(pendingJoinedTeamId_)) {
        const QString joined = pendingJoinedTeamId_;
        pendingJoinedTeamId_.clear();
        preferredTeamId_.clear();
        setActiveTeam(joined, true);
        return;
    }

    if (!activeTeamId_.isEmpty() && hasTeamRoot(activeTeamId_)) {
        setActiveTeam(activeTeamId_, false);
        return;
    }

    if (!preferredTeamId_.isEmpty() && hasTeamRoot(preferredTeamId_)) {
        const QString preferred = preferredTeamId_;
        preferredTeamId_.clear();
        setActiveTeam(preferred, false);
        return;
    }

    preferredTeamId_.clear();
    setActiveTeam(firstRootId, false);
}

bool TeamSelectorLabel::setActiveTeam(const QString& teamId, bool persist)
{
    if (!tree_ || teamId.isEmpty() || !hasTeamRoot(teamId)) {
        return false;
    }
    Backend* backend = tree_->backendInstance();
    BackendTeam* team = backend ? backend->getStorage().getTeamById(teamId) : nullptr;
    if (!backend || !team) {
        return false;
    }

    QModelIndex rootIndex;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item
            || item->data(0, ChannelTree::ItemKindRole).toInt()
                != ChannelTree::TeamItemKind) {
            continue;
        }
        item->setHidden(false);
        if (item->data(0, ChannelTree::ItemTeamIdRole).toString() == teamId) {
            rootIndex = tree_->model()->index(i, 0, QModelIndex());
        }
    }
    if (!rootIndex.isValid()) {
        return false;
    }

    QTreeWidgetItem* current = tree_->currentItem();
    if (current
        && current->data(0, ChannelTree::ItemTeamIdRole).toString() != teamId) {
        tree_->setCurrentItem(nullptr);
    }

    activeTeamId_ = teamId;
    preferredTeamId_.clear();
    tree_->setProperty("_mmqt_active_team_id", teamId);
    backend->setCurrentTeamContextId(teamId);

    // Make the TeamItem the QTreeView root instead of trying to paint it away.
    // Its category children become the visual top level while all existing
    // TeamItem parentage remains intact for sidebar state, DnD and factories.
    tree_->setRootIndex(rootIndex);

    setText(teamLabel(*team));
    setToolTip(tr("Switch team"));
    if (persist) {
        MLOptions::instance()->setValue(
            QString::fromLatin1(ActiveTeamSetting), activeTeamId_);
    }
    tree_->viewport()->update();
    updateTeamToolbarSelection();
    return true;
}

void TeamSelectorLabel::showPublicChannels()
{
    if (!tree_ || !tree_->backendInstance() || activeTeamId_.isEmpty()) {
        return;
    }

    Backend& backend = *tree_->backendInstance();
    BackendTeam* team = backend.getStorage().getTeamById(activeTeamId_);
    if (!team) {
        return;
    }

    TeamChannelsListDialog::showForTeam(backend, *team, tree_);
}

void TeamSelectorLabel::addAnotherTeam()
{
    if (!tree_ || !tree_->backendInstance()) {
        return;
    }

    QPointer<TeamSelectorLabel> guard(this);
    tree_->backendInstance()->retrieveJoinableTeams(
        [guard](QJsonArray response) {
            if (!guard || !guard->tree_ || !guard->tree_->backendInstance()) {
                return;
            }

            Backend& backend = *guard->tree_->backendInstance();
            QVector<QPair<QString, QString>> choices;
            choices.reserve(response.size());
            for (const QJsonValue& value : response) {
                const QJsonObject object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || backend.getStorage().getTeamById(id)) {
                    continue;
                }
                if (object.value(QStringLiteral("delete_at")).toVariant().toULongLong() != 0) {
                    continue;
                }
                choices.push_back(qMakePair(joinableTeamLabel(object), id));
            }

            std::sort(choices.begin(), choices.end(),
                      [](const auto& lhs, const auto& rhs) {
                return lhs.first.compare(rhs.first, Qt::CaseInsensitive) < 0;
            });

            if (choices.isEmpty()) {
                QMessageBox::information(guard, guard->tr("Add team"),
                                         guard->tr("No additional public teams are available."));
                return;
            }

            QStringList labels;
            labels.reserve(choices.size());
            for (const auto& choice : choices) {
                labels.push_back(choice.first);
            }

            bool accepted = false;
            const QString selected = QInputDialog::getItem(
                guard, guard->tr("Add team"), guard->tr("Team:"),
                labels, 0, false, &accepted);
            const int index = labels.indexOf(selected);
            if (!accepted || index < 0) {
                return;
            }

            guard->pendingJoinedTeamId_ = choices.at(index).second;
            backend.joinTeam(guard->pendingJoinedTeamId_);
        });
}

} // namespace Mattermost
