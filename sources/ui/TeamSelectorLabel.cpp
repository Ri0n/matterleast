#include "TeamSelectorLabel.h"

#include <algorithm>
#include <functional>
#include <vector>

#include <QAbstractItemModel>
#include <QAction>
#include <QEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelTree.h"

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

QAction* findLogoutAction(QMenu* menu)
{
    if (!menu) {
        return nullptr;
    }
    for (QAction* action : menu->actions()) {
        if (!action) {
            continue;
        }
        if (action->text() == QStringLiteral("Logout")) {
            return action;
        }
        if (QAction* nested = findLogoutAction(action->menu())) {
            return nested;
        }
    }
    return nullptr;
}

} // namespace

TeamSelectorLabel::TeamSelectorLabel(QWidget* parent)
    : ClickableLabel(parent)
    , preferredTeamId_(QSettings().value(QString::fromLatin1(ActiveTeamSetting)).toString())
{
    setMinimumHeight(18);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::NoTextInteraction);
    setAccessibleName(tr("Current team"));

    QFont labelFont = font();
    labelFont.setBold(true);
    setFont(labelFont);

    connect(this, &ClickableLabel::clicked, this,
            [this] { showTeamMenu(); });
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

void TeamSelectorLabel::showEvent(QShowEvent* event)
{
    ClickableLabel::showEvent(event);
    attachTree();
}

bool TeamSelectorLabel::eventFilter(QObject* watched, QEvent* event)
{
    if (tree_ && watched == tree_->viewport() && event) {
        switch (event->type()) {
        case QEvent::Paint:
        case QEvent::Show:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            enforceInactiveTeamVisibility();
            break;
        default:
            break;
        }
    }
    return ClickableLabel::eventFilter(watched, event);
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
    tree_->viewport()->installEventFilter(this);

    auto refreshLater = [this] {
        QTimer::singleShot(0, this, [this] { refreshTeams(); });
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
        if (firstRootId.isEmpty()) {
            firstRootId = item->data(0, ChannelTree::ItemTeamIdRole).toString();
        }
    }

    if (firstRootId.isEmpty()) {
        setText(tr("Team"));
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
        enforceInactiveTeamVisibility();
        if (BackendTeam* team = backend->getStorage().getTeamById(activeTeamId_)) {
            setText(teamLabel(*team) + QStringLiteral("  \u25BE"));
            setToolTip(tr("Switch team"));
        }
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

    activeTeamId_ = teamId;
    preferredTeamId_.clear();
    tree_->setProperty("_mmqt_active_team_id", teamId);
    backend->setCurrentTeamContextId(teamId);

    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item
            || item->data(0, ChannelTree::ItemKindRole).toInt()
                != ChannelTree::TeamItemKind) {
            continue;
        }
        const QString rootTeamId = item->data(0, ChannelTree::ItemTeamIdRole).toString();
        item->setHidden(rootTeamId != activeTeamId_);
    }

    QTreeWidgetItem* current = tree_->currentItem();
    if (current
        && current->data(0, ChannelTree::ItemTeamIdRole).toString() != activeTeamId_) {
        tree_->setCurrentItem(nullptr);
    }

    setText(teamLabel(*team) + QStringLiteral("  \u25BE"));
    setToolTip(tr("Switch team"));
    if (persist) {
        QSettings().setValue(QString::fromLatin1(ActiveTeamSetting), activeTeamId_);
    }
    tree_->viewport()->update();
    return true;
}

void TeamSelectorLabel::enforceInactiveTeamVisibility()
{
    if (!tree_ || activeTeamId_.isEmpty()) {
        return;
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item
            || item->data(0, ChannelTree::ItemKindRole).toInt()
                != ChannelTree::TeamItemKind) {
            continue;
        }
        if (item->data(0, ChannelTree::ItemTeamIdRole).toString() != activeTeamId_
            && !item->isHidden()) {
            item->setHidden(true);
        }
    }
}

void TeamSelectorLabel::showTeamMenu()
{
    attachTree();
    if (!tree_ || !tree_->backendInstance()) {
        return;
    }
    refreshTeams();

    Backend& backend = *tree_->backendInstance();
    std::vector<BackendTeam*> teams;
    teams.reserve(backend.getStorage().teams.size());
    for (auto& entry : backend.getStorage().teams) {
        if (hasTeamRoot(entry.first)) {
            teams.push_back(&entry.second);
        }
    }
    std::sort(teams.begin(), teams.end(), [](const BackendTeam* lhs, const BackendTeam* rhs) {
        return teamLabel(*lhs).compare(teamLabel(*rhs), Qt::CaseInsensitive) < 0;
    });

    QMenu menu(this);
    for (BackendTeam* team : teams) {
        QAction* action = menu.addAction(teamLabel(*team));
        action->setCheckable(true);
        action->setChecked(team->id == activeTeamId_);
        const QString teamId = team->id;
        connect(action, &QAction::triggered, this,
                [this, teamId] { setActiveTeam(teamId, true); });
    }

    if (!teams.empty()) {
        menu.addSeparator();
    }
    menu.addAction(tr("Add another team\u2026"), this,
                   [this] { addAnotherTeam(); });
    menu.addAction(tr("Log out"), this,
                   [this] { triggerLogout(); });

    menu.exec(mapToGlobal(QPoint(0, height())));
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

void TeamSelectorLabel::triggerLogout()
{
    QWidget* root = window();
    auto* menuButton = root
        ? root->findChild<QToolButton*>(QStringLiteral("toolButton"))
        : nullptr;
    QAction* logout = menuButton ? findLogoutAction(menuButton->menu()) : nullptr;
    if (logout) {
        logout->trigger();
    }
}

} // namespace Mattermost
