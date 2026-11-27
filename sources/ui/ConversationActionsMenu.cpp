#include "ConversationActionsMenu.h"

#include <functional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/SidebarChannelMovePolicy.h"
#include "channel-tree-dialogs/EditChannelPropertiesDialog.h"
#include "channel-tree-dialogs/CreateChannelDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "channel-tree-dialogs/ViewChannelMembersListDialog.h"
#include "chat-area/ChannelTopicEditDialog.h"
#include "info-dialogs/ChannelInfoDialog.h"
#include "info-dialogs/UserProfileDialog.h"
#include "mainwindow.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ConversationReference.h"
#include "navigation/MattermostUrlRouter.h"
#include "ui/IconUtils.h"

namespace Mattermost {
namespace {

BackendChannel* getChannel(Backend& backend, const QString& id)
{
    return backend.getStorage().getChannelById(id);
}

QString menuTeamId(Backend& backend, const BackendChannel& channel,
                   const QString& preferredTeamId)
{
    if (channel.team) {
        return channel.team->id;
    }
    if (!preferredTeamId.isEmpty()) {
        return preferredTeamId;
    }
    return backend.getCurrentTeamContextId();
}

QString sourceCategoryId(const SidebarTeamState& state, const QString& channelId)
{
    for (const QString& id : state.order) {
        const SidebarCategory* category = state.category(id);
        if (category && category->channelIds.contains(channelId)) {
            return id;
        }
    }
    return {};
}

void warnIfFailed(QPointer<QWidget> parent, const QString& title,
                  const QString& message, bool success)
{
    if (!success && parent) {
        QMessageBox::warning(parent, title, message);
    }
}

QAction* addAction(QMenu& menu, const QString& name, const QString& title,
                   const QString& iconPath, std::function<void()> handler)
{
    QAction* action = menu.addAction(title);
    action->setObjectName(name);
    if (!iconPath.isEmpty()) {
        IconUtils::setMenuActionIcon(*action, iconPath);
    }
    if (handler) {
        QObject::connect(action, &QAction::triggered, &menu,
                         [handler = std::move(handler)] { handler(); });
    }
    return action;
}

void openGroupConversionDialog(Backend& backend, const QString& channelId,
                               const QString& preferredTeam, QWidget* parent)
{
    if (!parent) return;
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("convertGroupMessageDialog"));
    dialog->setWindowTitle(QObject::tr("Convert to private channel"));
    auto* layout = new QVBoxLayout(dialog);
    auto* explanation = new QLabel(QObject::tr(
        "The group message will become a private channel in the selected team. "
        "Its message history and members are retained. This cannot be undone."), dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    auto* teams = new QComboBox(dialog);
    teams->setObjectName(QStringLiteral("conversionTeam"));
    auto* displayName = new QLineEdit(dialog);
    displayName->setObjectName(QStringLiteral("conversionDisplayName"));
    displayName->setMaxLength(64);
    auto* urlName = new QLineEdit(dialog);
    urlName->setObjectName(QStringLiteral("conversionUrlName"));
    urlName->setMaxLength(64);
    form->addRow(QObject::tr("Team:"), teams);
    form->addRow(QObject::tr("Display name:"), displayName);
    form->addRow(QObject::tr("URL name:"), urlName);
    layout->addLayout(form);
    auto* status = new QLabel(QObject::tr("Loading common teams…"), dialog);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* convert = buttons->button(QDialogButtonBox::Ok);
    convert->setText(QObject::tr("Convert"));
    convert->setEnabled(false);
    layout->addWidget(buttons);
    const QPointer<QDialog> dialogGuard(dialog);
    const QPointer<Backend> backendGuard(&backend);
    const auto validate = [teams, displayName, urlName, convert] {
        static const QRegularExpression validName(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
        convert->setEnabled(teams->currentIndex() >= 0 && teams->isEnabled()
            && !displayName->text().trimmed().isEmpty()
            && validName.match(urlName->text().trimmed()).hasMatch());
    };
    teams->setEnabled(false);
    QObject::connect(displayName, &QLineEdit::textChanged, dialog, [urlName, validate](const QString& text) {
        if (!urlName->isModified())
            urlName->setText(CreateChannelDialog::generatedChannelName(text));
        validate();
    });
    QObject::connect(urlName, &QLineEdit::textChanged, dialog, validate);
    QObject::connect(teams, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog,
        [dialogGuard, backendGuard, channelId, teams, displayName, urlName, status, validate] {
        if (!dialogGuard || !backendGuard || !teams->isEnabled()) return;
        const QString teamId = teams->currentData().toString();
        const QString name = urlName->text().trimmed();
        const QString display = displayName->text().trimmed();
        teams->setEnabled(false);
        displayName->setEnabled(false);
        urlName->setEnabled(false);
        validate();
        status->setText(QObject::tr("Converting…"));
        backendGuard->convertGroupMessageToChannel(channelId, teamId, name, display,
            [dialogGuard, backendGuard, channelId, teams, displayName, urlName, status, validate]
            (bool success, const QString& error) {
            if (!dialogGuard || !backendGuard) return;
            if (success) {
                dialogGuard->accept();
                if (backendGuard)
                    AppNavigationService::instance(*backendGuard).openChannel(channelId);
                return;
            }
            status->setText(error);
            teams->setEnabled(true);
            displayName->setEnabled(true);
            urlName->setEnabled(true);
            validate();
        });
    });
    dialog->open();
    backend.retrieveGroupConversionTeams(channelId,
        [dialogGuard, backendGuard, preferredTeam, teams, status, validate]
        (const QJsonArray& commonTeams, const QString& error) {
        if (!dialogGuard || !backendGuard) return;
        if (!error.isEmpty()) {
            status->setText(error);
            return;
        }
        for (const auto& value : commonTeams) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            const BackendTeam* team = backendGuard->getStorage().getTeamById(id);
            if (team && teams->findData(id) < 0)
                teams->addItem(team->display_name.isEmpty() ? team->name : team->display_name, id);
        }
        if (!teams->count()) {
            status->setText(QObject::tr("There is no common team for all group members."));
            return;
        }
        const int preferred = teams->findData(preferredTeam);
        if (preferred >= 0) teams->setCurrentIndex(preferred);
        teams->setEnabled(true);
        status->clear();
        validate();
    });
}

} // namespace

QString ConversationActionsMenu::channelWebUrl(Backend& backend,
                                                const BackendChannel& channel,
                                                const QString& preferredTeamId)
{
    const BackendTeam* team = channel.team;
    if (!team) {
        team = backend.getStorage().getTeamById(
            preferredTeamId.isEmpty()
                ? backend.getCurrentTeamContextId() : preferredTeamId);
    }
    if (!team) {
        return {};
    }

    MattermostUrlRoute::Kind routeKind = MattermostUrlRoute::Kind::LocalOther;
    QString target;
    switch (channel.type) {
    case BackendChannel::publicChannel:
    case BackendChannel::privateChannel:
        routeKind = MattermostUrlRoute::Kind::Channel;
        target = channel.name;
        break;
    case BackendChannel::directChannel:
        routeKind = MattermostUrlRoute::Kind::DirectMessage;
        if (const BackendUser* peer = backend.getStorage().getUserById(channel.name)) {
            target = peer->username;
        }
        break;
    case BackendChannel::groupChannel:
        routeKind = MattermostUrlRoute::Kind::GroupMessage;
        target = channel.name;
        break;
    default:
        return {};
    }

    const QString path =
        MattermostUrlRouter::conversationPath(routeKind, team->name, target);
    if (path.isEmpty()) {
        return {};
    }
    QString base = backend.networkSession().host();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return base.isEmpty() ? QString() : base + path;
}

void ConversationActionsMenu::populate(
    QMenu& menu, Backend& backend, const QString& channelId,
    const QString& teamId, QWidget* dialogParent, bool thread)
{
    // The caller owns the menu. Never capture a sidebar row or ChatArea:
    // reconciliation or tab changes can destroy them while a menu is open.
    Backend* backendPtr = &backend;
    const QPointer<QWidget> parentGuard(dialogParent);
    const auto activeChannel = [backendPtr, channelId] {
        return getChannel(*backendPtr, channelId);
    };
    const auto resolveTeam = [backendPtr, activeChannel, teamId] {
        const BackendChannel* channel = activeChannel();
        return channel ? menuTeamId(*backendPtr, *channel, teamId) : QString();
    };
    QObject::connect(&menu, &QMenu::aboutToShow, &menu,
                     [&menu] { IconUtils::refreshMenuActionIcons(menu); });

    if (!thread) {
        addAction(menu, QStringLiteral("openConversationInTabAction"),
                  QObject::tr("Open in new tab"), QStringLiteral(":/icons/open-in-new-tab"),
                  [backendPtr, channelId] {
            AppNavigationService::instance(*backendPtr).openChannelInTab(channelId);
        });
    }

    addAction(menu, QStringLiteral("searchConversationAction"),
              QObject::tr("Search in conversation"), QStringLiteral(":/icons/search"),
              [backendPtr, channelId] {
        // Both the header and the sidebar route through the same search page.
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* window = qobject_cast<MainWindow*>(widget);
            if (window && &window->backendInstance() == backendPtr) {
                window->openMessageSearchInChannel(channelId);
                break;
            }
        }
    });

    QAction* copyReference = addAction(
        menu, QStringLiteral("copyConversationReferenceAction"),
        QObject::tr("Copy reference"), QStringLiteral(":/icons/copy"),
        [backendPtr, activeChannel] {
        if (const BackendChannel* channel = activeChannel()) {
            const QString reference = ConversationReference::copyText(*backendPtr, *channel);
            if (!reference.isEmpty()) {
                QApplication::clipboard()->setText(reference);
            }
        }
    });
    QAction* copyLink = addAction(
        menu, QStringLiteral("copyConversationLinkAction"),
        QObject::tr("Copy link to chat"), QStringLiteral(":/icons/link"),
        [backendPtr, activeChannel, resolveTeam] {
        if (const BackendChannel* channel = activeChannel()) {
            const QString link =
                ConversationActionsMenu::channelWebUrl(
                    *backendPtr, *channel, resolveTeam());
            if (!link.isEmpty()) {
                QApplication::clipboard()->setText(link);
            }
        }
    });
    QObject::connect(&menu, &QMenu::aboutToShow, &menu,
                     [copyReference, copyLink, activeChannel, backendPtr, resolveTeam] {
        const BackendChannel* channel = activeChannel();
        const QString reference = channel
            ? ConversationReference::copyText(*backendPtr, *channel) : QString();
        copyReference->setVisible(!reference.isEmpty());
        copyLink->setEnabled(channel
            && !ConversationActionsMenu::channelWebUrl(
                *backendPtr, *channel, resolveTeam()).isEmpty());
    });

    if (thread) {
        return; // Thread-only AI / follow actions are owned by ChatArea.
    }

    menu.addSeparator();

    QAction* favorite = addAction(
        menu, QStringLiteral("toggleConversationFavoriteAction"),
        QObject::tr("Add to Favorites"), QStringLiteral(":/icons/heart-outline"),
        [backendPtr, channelId, resolveTeam, parentGuard] {
        BackendChannel* channel = getChannel(*backendPtr, channelId);
        if (!channel) {
            return;
        }
        const QString currentTeam = resolveTeam();
        auto& sidebar = SidebarService::instance(*backendPtr);
        const bool next = !sidebar.isChannelFavorite(*channel, currentTeam);
        sidebar.setChannelFavorite(currentTeam, channelId, next,
            [parentGuard](bool ok) {
            warnIfFailed(parentGuard, QObject::tr("Favorites"),
                         QObject::tr("Could not update Favorites."), ok);
        });
    });

    QAction* muted = addAction(
        menu, QStringLiteral("toggleConversationMuteAction"),
        QObject::tr("Mute conversation"), QStringLiteral(":/icons/bell"),
        [backendPtr, activeChannel] {
        if (BackendChannel* channel = activeChannel()) {
            auto& service = SidebarService::instance(*backendPtr);
            service.setChannelMuted(*channel, !service.isChannelMuted(*channel));
        }
    });
    QObject::connect(&menu, &QMenu::aboutToShow, &menu,
                     [favorite, muted, activeChannel, resolveTeam, backendPtr] {
        const BackendChannel* channel = activeChannel();
        if (!channel) {
            favorite->setEnabled(false);
            muted->setEnabled(false);
            return;
        }
        auto& service = SidebarService::instance(*backendPtr);
        const QString currentTeam = resolveTeam();
        const SidebarTeamState* state = service.teamState(currentTeam);
        const bool available = state
            && state->categoryByType(QStringLiteral("favorites"));
        const bool isFavorite = available
            && service.isChannelFavorite(*channel, currentTeam);
        favorite->setEnabled(available);
        favorite->setText(isFavorite ? QObject::tr("Remove from Favorites")
                                     : QObject::tr("Add to Favorites"));
        IconUtils::setMenuActionIcon(
            *favorite, isFavorite ? QStringLiteral(":/icons/heart-filled")
                                  : QStringLiteral(":/icons/heart-outline"));
        const bool isMuted = service.isChannelMuted(*channel);
        muted->setText(isMuted ? QObject::tr("Unmute conversation")
                               : QObject::tr("Mute conversation"));
    });

    QMenu* categories = menu.addMenu(QObject::tr("Move to category"));
    categories->setObjectName(QStringLiteral("moveConversationToCategoryMenu"));
    categories->menuAction()->setObjectName(
        QStringLiteral("moveConversationToCategoryMenu"));
    IconUtils::setMenuActionIcon(*categories->menuAction(),
                                 QStringLiteral(":/icons/menu-folder"));
    QObject::connect(categories, &QMenu::aboutToShow, categories,
                     [categories, backendPtr, channelId, resolveTeam, parentGuard] {
        categories->clear();
        auto& sidebar = SidebarService::instance(*backendPtr);
        const QString currentTeam = resolveTeam();
        const SidebarTeamState* state = sidebar.teamState(currentTeam);
        if (state) {
            const QString sourceId = sourceCategoryId(*state, channelId);
            for (const QString& id : state->order) {
                const SidebarCategory* target = state->category(id);
                if (!target || !sidebarCategoryIsMoveTarget(
                        target->type, target->id, sourceId)) {
                    continue;
                }
                const QString label = !target->displayName.isEmpty()
                    ? target->displayName
                    : target->type == QLatin1String("favorites")
                        ? QObject::tr("Favorites") : QObject::tr("Category");
                QAction* action = categories->addAction(label);
                IconUtils::setMenuActionIcon(
                    *action, target->type == QLatin1String("favorites")
                        ? QStringLiteral(":/icons/heart-outline")
                        : QStringLiteral(":/icons/menu-folder"));
                QObject::connect(action, &QAction::triggered, categories,
                                 [backendPtr, currentTeam, channelId,
                                  destinationId = id, parentGuard] {
                    auto& service = SidebarService::instance(*backendPtr);
                    if (const SidebarTeamState* snapshot =
                            service.teamState(currentTeam)) {
                        if (const SidebarCategory* destination =
                                snapshot->category(destinationId)) {
                            if (destination->type == QLatin1String("favorites")) {
                                service.setChannelFavorite(
                                    currentTeam, channelId, true,
                                    [parentGuard](bool ok) {
                                    warnIfFailed(parentGuard, QObject::tr("Favorites"),
                                                 QObject::tr("Could not update Favorites."), ok);
                                });
                                return;
                            }
                        }
                    }
                    service.moveChannelToCategory(
                        currentTeam, channelId, destinationId,
                        [parentGuard](bool ok) {
                        warnIfFailed(parentGuard, QObject::tr("Move to category"),
                                     QObject::tr("Could not move this conversation."), ok);
                    });
                });
            }
        }
        if (!categories->actions().isEmpty()) {
            categories->addSeparator();
        }
        QAction* create = categories->addAction(QObject::tr("Create new category…"));
        IconUtils::setMenuActionIcon(*create, QStringLiteral(":/icons/add"));
        create->setEnabled(state != nullptr);
        QObject::connect(create, &QAction::triggered, categories,
                         [backendPtr, channelId, currentTeam, parentGuard] {
            bool accepted = false;
            const QString name = QInputDialog::getText(
                parentGuard, QObject::tr("Create category"),
                QObject::tr("Category name:"), QLineEdit::Normal,
                QString(), &accepted).trimmed();
            if (!accepted || name.isEmpty()) {
                return;
            }
            auto& service = SidebarService::instance(*backendPtr);
            const SidebarTeamState* currentState = service.teamState(currentTeam);
            const QString sourceId = currentState
                ? sourceCategoryId(*currentState, channelId) : QString();
            service.createCategory(
                currentTeam, name,
                [backendPtr, channelId, currentTeam, parentGuard](
                    const SidebarCategory& newCategory) {
                SidebarService::instance(*backendPtr).moveChannelToCategory(
                    currentTeam, channelId, newCategory.id,
                    [parentGuard](bool ok) {
                    warnIfFailed(parentGuard, QObject::tr("Move to category"),
                                 QObject::tr("Could not move this conversation."), ok);
                });
            }, sourceId, [parentGuard] {
                warnIfFailed(parentGuard, QObject::tr("Create category"),
                             QObject::tr("Could not create this category."), false);
            });
        });
    });

    QAction* removeCategory = addAction(
        menu, QStringLiteral("removeConversationFromCategoryAction"),
        QObject::tr("Remove from category"), QStringLiteral(":/icons/menu-folder"),
        [backendPtr, channelId, resolveTeam, parentGuard] {
        SidebarService::instance(*backendPtr).removeChannelFromCustomCategory(
            resolveTeam(), channelId, [parentGuard](bool ok) {
            warnIfFailed(parentGuard, QObject::tr("Remove from category"),
                         QObject::tr("Could not remove this conversation."), ok);
        });
    });
    QObject::connect(&menu, &QMenu::aboutToShow, &menu,
                     [removeCategory, categories, backendPtr, channelId, resolveTeam] {
        const SidebarTeamState* state =
            SidebarService::instance(*backendPtr).teamState(resolveTeam());
        categories->menuAction()->setEnabled(state != nullptr);
        const SidebarCategory* source = state
            ? state->category(sourceCategoryId(*state, channelId)) : nullptr;
        removeCategory->setVisible(source && source->type == QLatin1String("custom"));
    });

    BackendChannel* original = activeChannel();
    const bool direct = original && original->type == BackendChannel::directChannel;
    const bool teamChannel = original
        && (original->type == BackendChannel::publicChannel
            || original->type == BackendChannel::privateChannel);

    if (direct) {
        menu.addSeparator();
        QAction* profile = addAction(
            menu, QStringLiteral("viewConversationProfileAction"),
            QObject::tr("View Profile"), QStringLiteral(":/icons/members"),
            [backendPtr, activeChannel, parentGuard] {
            const BackendChannel* channel = activeChannel();
            BackendUser* peer = channel
                ? backendPtr->getStorage().getUserById(channel->name) : nullptr;
            if (peer) {
                auto* dialog = new UserProfileDialog(*backendPtr, *peer, parentGuard);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->show();
            }
        });
        QObject::connect(&menu, &QMenu::aboutToShow, &menu,
                         [profile, activeChannel, backendPtr] {
            const BackendChannel* channel = activeChannel();
            profile->setEnabled(channel
                && backendPtr->getStorage().getUserById(channel->name));
        });
    }

    const bool group = original && original->type == BackendChannel::groupChannel;
    if (group) {
        auto* separator = menu.addSeparator();
        auto* convert = addAction(menu, QStringLiteral("convertGroupMessageAction"),
            QObject::tr("Convert to private channel…"), QStringLiteral(":/icons/edit"),
            [backendPtr, activeChannel, resolveTeam, parentGuard] {
            const BackendChannel* channel = activeChannel();
            if (channel && channel->type == BackendChannel::groupChannel)
                openGroupConversionDialog(*backendPtr, channel->id, resolveTeam(), parentGuard);
        });
        const auto update = [activeChannel, separator, convert] {
            const auto* channel = activeChannel();
            const bool visible = channel && channel->type == BackendChannel::groupChannel;
            separator->setVisible(visible);
            convert->setVisible(visible);
        };
        QObject::connect(&menu, &QMenu::aboutToShow, &menu, update);
        update();
    }

    if (teamChannel || group) {
        const int firstTeamAction = menu.actions().size();
        menu.addSeparator();
        addAction(menu, QStringLiteral("viewChannelDetailsAction"),
                  QObject::tr("View channel details"), QStringLiteral(":/icons/menu-info"),
                  [activeChannel, parentGuard] {
            if (BackendChannel* channel = activeChannel()) {
                auto* dialog = new ChannelInfoDialog(*channel, parentGuard);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->show();
            }
        });
        addAction(menu, QStringLiteral("viewChannelMembersAction"),
                  QObject::tr("View channel members"), QStringLiteral(":/icons/members"),
                  [backendPtr, activeChannel, parentGuard] {
            if (BackendChannel* channel = activeChannel()) {
                auto* dialog =
                    new ViewChannelMembersListDialog(*backendPtr, *channel, parentGuard);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->show();
            }
        });
        addAction(menu, QStringLiteral("addChannelMemberAction"),
                  QObject::tr("Add user to channel…"), QStringLiteral(":/icons/add"),
                  [backendPtr, activeChannel, parentGuard] {
            if (BackendChannel* channel = activeChannel()) {
                UserSearchDialog::showChannelMemberPicker(
                    *backendPtr, *channel, parentGuard);
            }
        });
        addAction(menu, QStringLiteral("editChannelTopicAction"),
                  QObject::tr("Edit channel topic…"), QStringLiteral(":/icons/edit"),
                  [backendPtr, activeChannel, parentGuard] {
            if (BackendChannel* channel = activeChannel()) {
                auto* dialog = new ChannelTopicEditDialog(
                    *backendPtr, *channel, parentGuard);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->open();
            }
        });
        addAction(menu, QStringLiteral("editChannelPropertiesAction"),
                  QObject::tr("Edit channel properties…"), QStringLiteral(":/icons/edit"),
                  [backendPtr, activeChannel, parentGuard] {
            if (BackendChannel* channel = activeChannel()) {
                auto* dialog = new EditChannelPropertiesDialog(*channel, parentGuard);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                const QPointer<BackendChannel> liveChannel(channel);
                QObject::connect(dialog, &QDialog::accepted, dialog,
                                 [backendPtr, liveChannel, dialog] {
                    if (liveChannel) {
                        backendPtr->editChannelProperties(
                            *liveChannel, dialog->getNewProperties());
                    }
                });
                dialog->open();
            }
        });
        menu.addSeparator();
        addAction(menu, QStringLiteral("leaveConversationAction"),
                  QObject::tr("Leave channel"), QStringLiteral(":/icons/menu-logout"),
                  [backendPtr, activeChannel, parentGuard] {
            BackendChannel* channel = activeChannel();
            if (channel && QMessageBox::question(
                    parentGuard, QObject::tr("Leave channel"),
                    QObject::tr("Leave the channel “%1”?")
                        .arg(channel->display_name)) == QMessageBox::Yes) {
                backendPtr->leaveChannel(*channel);
            }
        });
        // Header menus persist after a GM is converted; sidebar menus are
        // recreated. Keep the same factory and refresh action visibility.
        const auto teamActions = menu.actions().mid(firstTeamAction);
        const auto update = [activeChannel, teamActions] {
            const BackendChannel* channel = activeChannel();
            const bool visible = channel && (channel->type == BackendChannel::publicChannel
                || channel->type == BackendChannel::privateChannel);
            for (auto* action : teamActions)
                action->setVisible(visible);
        };
        QObject::connect(&menu, &QMenu::aboutToShow, &menu, update);
        update();
    }
}

} // namespace Mattermost
