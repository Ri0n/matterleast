#pragma once

#include "ClickableLabel.h"

#include <QString>

class QEvent;
class QShowEvent;

namespace Mattermost {

class ChannelTree;

/**
 * Compact active-team selector used by the left sidebar.
 *
 * ChannelTree still owns one logical root item per Mattermost team because a
 * substantial amount of category state is naturally scoped by that root. This
 * label turns those roots into presentation state: only the active team's root
 * is exposed and the root row itself is rendered with zero height by the tree
 * delegate. To the user the sidebar therefore starts directly with categories.
 */
class TeamSelectorLabel final : public ClickableLabel
{
    Q_OBJECT

public:
    explicit TeamSelectorLabel(QWidget* parent = nullptr);

    QString activeTeamId() const { return activeTeamId_; }

    /** Switch the selector belonging to context's window, if one exists. */
    static bool activateTeam(QWidget* context, const QString& teamId);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void attachTree();
    void refreshTeams();
    bool setActiveTeam(const QString& teamId, bool persist);
    void enforceInactiveTeamVisibility();
    bool hasTeamRoot(const QString& teamId) const;
    void showTeamMenu();
    void addAnotherTeam();
    void triggerLogout();

    ChannelTree* tree_ = nullptr;
    QString activeTeamId_;
    QString preferredTeamId_;
    QString pendingJoinedTeamId_;
};

} // namespace Mattermost
