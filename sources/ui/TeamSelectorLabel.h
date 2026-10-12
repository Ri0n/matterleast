#pragma once

#include <QToolButton>
#include <QString>
#include <QPointer>

class QEvent;
class QPaintEvent;
class QShowEvent;
class QHBoxLayout;
class QWidget;

namespace Mattermost {

class ChannelTree;

/**
 * Compact active-team selector used by the left sidebar.
 *
 * ChannelTree keeps one logical root item per Mattermost team. The selector
 * installs the active TeamItem as QTreeView's root index, so that logical root
 * remains available to the existing category/DnD code but is never rendered;
 * its categories are the visual top level of the sidebar. Team-scoped actions
 * that used to live on the now-unrendered TeamItem context menu therefore also
 * need a discoverable entry point in this selector menu.
 */
class TeamSelectorLabel final : public QToolButton
{
    Q_OBJECT

public:
    explicit TeamSelectorLabel(QWidget* parent = nullptr);

    QString activeTeamId() const { return activeTeamId_; }

    QWidget* createTeamToolbar(QWidget* parent);

    /** Switch the selector belonging to context's window, if one exists. */
    static bool activateTeam(QWidget* context, const QString& teamId);

protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void attachTree();
    void refreshTeams();
    void rebuildTeamToolbar();
    bool setActiveTeam(const QString& teamId, bool persist);
    bool hasTeamRoot(const QString& teamId) const;
    void showPublicChannels();
    void addAnotherTeam();

    ChannelTree* tree_ = nullptr;
    QPointer<QWidget> _teamToolbar;
    QPointer<QWidget> _teamButtonsHost;
    QHBoxLayout* _teamButtonsLayout = nullptr;
    QString activeTeamId_;
    QString preferredTeamId_;
    QString pendingJoinedTeamId_;
};

} // namespace Mattermost
