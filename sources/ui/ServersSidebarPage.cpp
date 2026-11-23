#include "ServersSidebarPage.h"

#include <algorithm>
#include <utility>

#include <QAbstractItemView>
#include <QApplication>
#include <QFrame>
#include <QIcon>
#include <QPainter>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <functional>
#include "ui/IconUtils.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>

#include "backend/AccountManager.h"
#include "backend/Account.h"
#include "workspaces/WorkspaceLayout.h"
#include "workspaces/WorkspaceTreeModel.h"

namespace Mattermost {
namespace {

QString generatedId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Keep the QtCore model independent of the Widgets presentation. Its
// ConnectionStateRole also lets a future QML tree render the same state.
class ServerStatusDelegate final : public QStyledItemDelegate
{
public:
    using Action = std::function<void(const QModelIndex&)>;
    explicit ServerStatusDelegate(Action edit, Action connectOrDisconnect,
                                  QObject* parent)
        : QStyledItemDelegate(parent), _edit(std::move(edit)),
          _connectOrDisconnect(std::move(connectOrDisconnect))
    {
    }

    static AccountConnectionState connectionState(const QModelIndex& index)
    {
        return static_cast<AccountConnectionState>(
            index.data(WorkspaceTreeModel::ConnectionStateRole).toInt());
    }

    static bool canToggleConnection(const QModelIndex& index)
    {
        switch (connectionState(index)) {
        case AccountConnectionState::Connected:
        case AccountConnectionState::Disconnected:
        case AccountConnectionState::Unavailable:
            return true;
        default:
            return false;
        }
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem decorated(option);
        initStyleOption(&decorated, index);
        const int kind = index.data(WorkspaceTreeModel::NodeKindRole).toInt();
        const bool account = kind == WorkspaceTreeModel::AccountNode;
        if (account || kind == WorkspaceTreeModel::PendingNode) {
            const auto state = kind == WorkspaceTreeModel::PendingNode
                ? AccountConnectionState::Connecting : connectionState(index);
            QColor color;
            switch (state) {
            case AccountConnectionState::Connected:
                color = QColor(38, 165, 102); break;
            case AccountConnectionState::Connecting:
            case AccountConnectionState::SetupRequired:
                color = QColor(217, 150, 33); break;
            case AccountConnectionState::Unavailable:
            case AccountConnectionState::AuthenticationRequired:
                color = QColor(199, 79, 68); break;
            case AccountConnectionState::Disconnected:
                color = option.palette.color(QPalette::Mid); break;
            }
            QPixmap marker(21, 21);
            marker.fill(Qt::transparent);
            {
                QPainter icon(&marker);
                icon.setRenderHint(QPainter::Antialiasing);
                icon.scale(1.5, 1.5);
                icon.setPen(QPen(color, 1.5));
                icon.setBrush(state == AccountConnectionState::Disconnected
                    ? Qt::NoBrush : QBrush(color));
                icon.drawEllipse(QRectF(2.5, 2.5, 9, 9));
                if (state == AccountConnectionState::Connected) {
                    icon.setPen(QPen(Qt::white, 1.5, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
                    icon.drawLine(QPointF(4.5, 7), QPointF(6.5, 9));
                    icon.drawLine(QPointF(6.5, 9), QPointF(10, 5.5));
                }
            }
            decorated.icon = QIcon(marker);
            decorated.decorationSize = QSize(21, 21);
            decorated.features |= QStyleOptionViewItem::HasDecoration;
        }
        if (account)
            decorated.rect.adjust(0, 0, -2 * ActionHitWidth, 0);
        QStyle* style = decorated.widget ? decorated.widget->style()
                                        : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &decorated, painter,
                           decorated.widget);
        if (!account)
            return;

        const auto draw = [&](int slot, const QString& path, bool enabled) {
            const QRect hit = actionRect(option.rect, slot);
            const QRect iconRect(hit.center().x() - ActionExtent / 2,
                                 hit.center().y() - ActionExtent / 2,
                                 ActionExtent, ActionExtent);
            painter->save();
            painter->setOpacity(painter->opacity() *
                (enabled ? (option.state.testFlag(QStyle::State_MouseOver)
                    ? 1.0 : 0.48) : 0.22));
            IconUtils::tintedSymbolicIcon(
                path, option.palette.color(QPalette::Text)).paint(painter, iconRect);
            painter->restore();
        };
        // Settings are anchored to the right edge, exactly like the category
        // accessory slot. Connection control occupies the adjacent slot.
        draw(0, QStringLiteral(":/icons/menu-settings"), true);
        draw(1, QStringLiteral(":/icons/menu-power"), canToggleConnection(index));
    }

    bool editorEvent(QEvent* event, QAbstractItemModel*,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        if (index.data(WorkspaceTreeModel::NodeKindRole).toInt()
            != WorkspaceTreeModel::AccountNode)
            return false;
        if (event->type() != QEvent::MouseButtonPress
            && event->type() != QEvent::MouseButtonRelease
            && event->type() != QEvent::MouseButtonDblClick)
            return false;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        if (actionRect(option.rect, 0).contains(mouse->pos())) {
            if (event->type() == QEvent::MouseButtonRelease && _edit)
                _edit(index);
            return true;
        }
        if (actionRect(option.rect, 1).contains(mouse->pos())) {
            if (event->type() == QEvent::MouseButtonRelease
                && canToggleConnection(index) && _connectOrDisconnect)
                _connectOrDisconnect(index);
            return true;
        }
        return false;
    }

    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override
    {
        if (index.data(WorkspaceTreeModel::NodeKindRole).toInt()
            == WorkspaceTreeModel::AccountNode) {
            QString label;
            if (actionRect(option.rect, 0).contains(event->pos()))
                label = QObject::tr("Edit server");
            else if (actionRect(option.rect, 1).contains(event->pos())) {
                const auto state = connectionState(index);
                if (state == AccountConnectionState::Connected)
                    label = QObject::tr("Disconnect");
                else if (canToggleConnection(index))
                    label = QObject::tr("Connect");
                else
                    label = QObject::tr("Enter credentials in server settings");
            }
            if (!label.isEmpty()) {
                QToolTip::showText(event->globalPos(), label, view);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        // QTreeView uses uniform row heights. Even if the first row is a
        // profile rather than an account, reserve enough room for the 21px
        // status marker and its vertical margins.
        hint.setHeight(qMax(hint.height(), 30));
        return hint;
    }
private:
    static constexpr int ActionExtent = 20;
    static constexpr int ActionHitWidth = 30;
    static QRect actionRect(const QRect& row, int slot)
    {
        return QRect(row.right() - (slot + 1) * ActionHitWidth + 1, row.top(),
                     ActionHitWidth, row.height());
    }
    Action _edit;
    Action _connectOrDisconnect;
};

// Edits are staged in a plain WorkspaceLayout and committed only once on
// acceptance. No view reparenting, Backend restart, or keyring action can be
// triggered by an intermediate checkbox edit.
class WorkspaceLayoutDialog final : public QDialog
{
public:
    explicit WorkspaceLayoutDialog(AccountManager& manager, QWidget* parent)
        : QDialog(parent), _manager(manager), _draft(manager.layout())
    {
        setWindowTitle(tr("Server layout"));
        resize(480, 530);
        auto* outer = new QVBoxLayout(this);
        auto* description = new QLabel(
            tr("Profiles define which servers stay connected together. "
               "Views choose the visible subset and preserve their own tabs."), this);
        description->setWordWrap(true);
        outer->addWidget(description);

        auto* profileRow = new QHBoxLayout;
        _profiles = new QComboBox(this);
        _profiles->setObjectName(QStringLiteral("serverLayoutProfiles"));
        profileRow->addWidget(new QLabel(tr("Profile"), this));
        profileRow->addWidget(_profiles, 1);
        auto* addProfile = new QToolButton(this);
        addProfile->setText(QStringLiteral("+"));
        addProfile->setToolTip(tr("New profile"));
        auto* editProfile = new QToolButton(this);
        editProfile->setText(QStringLiteral("…"));
        editProfile->setToolTip(tr("Rename profile or choose connected servers"));
        auto* removeProfile = new QToolButton(this);
        removeProfile->setText(QStringLiteral("−"));
        removeProfile->setToolTip(tr("Delete profile"));
        auto* upProfile = new QToolButton(this);
        upProfile->setText(QStringLiteral("↑"));
        upProfile->setToolTip(tr("Move profile up"));
        auto* downProfile = new QToolButton(this);
        downProfile->setText(QStringLiteral("↓"));
        downProfile->setToolTip(tr("Move profile down"));
        profileRow->addWidget(upProfile);
        profileRow->addWidget(downProfile);
        profileRow->addWidget(addProfile);
        profileRow->addWidget(editProfile);
        profileRow->addWidget(removeProfile);
        outer->addLayout(profileRow);

        auto* viewRow = new QHBoxLayout;
        _views = new QComboBox(this);
        _views->setObjectName(QStringLiteral("serverLayoutViews"));
        viewRow->addWidget(new QLabel(tr("View"), this));
        viewRow->addWidget(_views, 1);
        auto* addView = new QToolButton(this);
        addView->setText(QStringLiteral("+"));
        addView->setToolTip(tr("New view"));
        auto* removeView = new QToolButton(this);
        removeView->setText(QStringLiteral("−"));
        removeView->setToolTip(tr("Delete view"));
        auto* upView = new QToolButton(this);
        upView->setText(QStringLiteral("↑"));
        upView->setToolTip(tr("Move view up"));
        auto* downView = new QToolButton(this);
        downView->setText(QStringLiteral("↓"));
        downView->setToolTip(tr("Move view down"));
        viewRow->addWidget(upView);
        viewRow->addWidget(downView);
        viewRow->addWidget(addView);
        viewRow->addWidget(removeView);
        outer->addLayout(viewRow);

        auto* form = new QFormLayout;
        _viewName = new QLineEdit(this);
        _viewName->setObjectName(QStringLiteral("serverLayoutViewName"));
        form->addRow(tr("View name"), _viewName);
        outer->addLayout(form);
        auto* memberLabel = new QLabel(tr("Servers visible in this view (in sidebar order)"), this);
        memberLabel->setWordWrap(true);
        outer->addWidget(memberLabel);
        _members = new QListWidget(this);
        _members->setObjectName(QStringLiteral("serverLayoutMembers"));
        outer->addWidget(_members, 1);

        auto* order = new QHBoxLayout;
        auto* up = new QPushButton(tr("Move up"), this);
        auto* down = new QPushButton(tr("Move down"), this);
        order->addWidget(up);
        order->addWidget(down);
        order->addStretch();
        outer->addLayout(order);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this,
                &WorkspaceLayoutDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(_profiles, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this] { rebuildViews(); });
        connect(_views, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this] { rebuildMembers(); });
        connect(_viewName, &QLineEdit::textEdited, this,
                [this](const QString& name) {
            if (auto* view = selectedView()) view->name = name;
        });
        connect(_members, &QListWidget::itemChanged, this,
                [this](QListWidgetItem* item) {
            if (auto* view = selectedView()) {
                const QString id = item->data(Qt::UserRole).toString();
                view->accountIds.removeAll(id);
                if (item->checkState() == Qt::Checked) {
                    int position = 0;
                    for (int i = 0; i < _members->row(item); ++i) {
                        if (_members->item(i)->checkState() == Qt::Checked)
                            ++position;
                    }
                    view->accountIds.insert(std::min(position, int(view->accountIds.size())), id);
                }
            }
        });
        connect(addView, &QToolButton::clicked, this, [this] { addNewView(); });
        connect(upView, &QToolButton::clicked, this, [this] { moveView(-1); });
        connect(downView, &QToolButton::clicked, this, [this] { moveView(1); });
        connect(upProfile, &QToolButton::clicked, this, [this] { moveProfile(-1); });
        connect(downProfile, &QToolButton::clicked, this, [this] { moveProfile(1); });
        connect(removeView, &QToolButton::clicked, this, [this] { deleteView(); });
        connect(addProfile, &QToolButton::clicked, this, [this] { addNewProfile(); });
        connect(editProfile, &QToolButton::clicked, this, [this] { editSelectedProfile(); });
        connect(removeProfile, &QToolButton::clicked, this, [this] { deleteProfile(); });
        connect(up, &QPushButton::clicked, this, [this] { moveMember(-1); });
        connect(down, &QPushButton::clicked, this, [this] { moveMember(1); });

        QString activeProfile;
        for (const auto& view : _draft.views)
            if (view.id == _draft.activeViewId) {
                activeProfile = view.profileId;
                break;
            }
        rebuildProfiles(activeProfile);
        rebuildViews(_draft.activeViewId);
    }

    void accept() override
    {
        QString error;
        if (!_draft.validate(&error) || !_manager.applyLayoutEdits(_draft, &error)) {
            QMessageBox::warning(this, tr("Invalid server layout"), error);
            return;
        }
        QDialog::accept();
    }

private:
    WorkspaceLayout::Profile* selectedProfile()
    {
        const QString id = _profiles->currentData().toString();
        for (auto& profile : _draft.profiles)
            if (profile.id == id) return &profile;
        return nullptr;
    }
    WorkspaceLayout::View* selectedView()
    {
        const QString id = _views->currentData().toString();
        for (auto& view : _draft.views)
            if (view.id == id) return &view;
        return nullptr;
    }
    void rebuildProfiles(const QString& preferred = {})
    {
        const QSignalBlocker blocker(_profiles);
        const QString id = preferred.isEmpty() ? _profiles->currentData().toString()
                                               : preferred;
        _profiles->clear();
        for (const auto& profile : _draft.profiles)
            _profiles->addItem(profile.name, profile.id);
        const int index = _profiles->findData(id);
        _profiles->setCurrentIndex(index >= 0 ? index : 0);
        rebuildViews();
    }
    void rebuildViews(const QString& preferred = {})
    {
        const QSignalBlocker blocker(_views);
        const QString id = preferred.isEmpty() ? _views->currentData().toString()
                                               : preferred;
        _views->clear();
        if (auto* profile = selectedProfile()) {
            for (const auto& view : _draft.views)
                if (view.profileId == profile->id)
                    _views->addItem(view.name, view.id);
        }
        const int index = _views->findData(id);
        _views->setCurrentIndex(index >= 0 ? index : 0);
        rebuildMembers();
    }
    void rebuildMembers()
    {
        const QSignalBlocker blocker(_members);
        _members->clear();
        auto* view = selectedView();
        auto* profile = selectedProfile();
        _viewName->setEnabled(view);
        _viewName->setText(view ? view->name : QString());
        if (!view || !profile) return;

        QStringList ordered = view->accountIds;
        for (const QString& id : profile->accountIds)
            if (!ordered.contains(id)) ordered.append(id);
        for (const QString& id : ordered) {
            const auto* account = _draft.account(id);
            if (!account) continue;
            auto* item = new QListWidgetItem(account->name, _members);
            item->setData(Qt::UserRole, id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(view->accountIds.contains(id)
                ? Qt::Checked : Qt::Unchecked);
        }
    }
    void addNewView()
    {
        auto* profile = selectedProfile();
        if (!profile || profile->accountIds.isEmpty()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New view"),
            tr("View name"), QLineEdit::Normal, tr("My view"), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        WorkspaceLayout::View view;
        view.id = generatedId();
        view.profileId = profile->id;
        view.name = name;
        view.accountIds.append(profile->accountIds.first());
        const QString id = view.id;
        _draft.views.append(std::move(view));
        rebuildViews(id);
    }
    void deleteView()
    {
        auto* view = selectedView();
        if (!view) return;
        const QString viewId = view->id;
        const QString profileId = view->profileId;
        int count = 0;
        for (const auto& item : _draft.views)
            count += item.profileId == profileId;
        if (count <= 1) {
            QMessageBox::information(this, tr("Cannot delete view"),
                                     tr("Every profile needs at least one view."));
            return;
        }
        for (int i = 0; i < _draft.views.size(); ++i)
            if (_draft.views[i].id == viewId) {
                _draft.views.remove(i);
                break;
            }
        if (_draft.activeViewId == viewId)
            for (const auto& item : _draft.views)
                if (item.profileId == profileId) {
                    _draft.activeViewId = item.id;
                    break;
                }
        rebuildViews();
    }
    void addNewProfile()
    {
        if (_draft.accounts.isEmpty()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New profile"),
            tr("Profile name"), QLineEdit::Normal, tr("New profile"), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        WorkspaceLayout::Profile profile;
        profile.id = generatedId();
        profile.name = name;
        profile.accountIds.append(_draft.accounts.first().id);
        WorkspaceLayout::View view;
        view.id = generatedId();
        view.profileId = profile.id;
        view.name = name;
        view.accountIds = profile.accountIds;
        const QString id = profile.id;
        _draft.profiles.append(std::move(profile));
        _draft.views.append(std::move(view));
        rebuildProfiles(id);
    }
    void deleteProfile()
    {
        auto* profile = selectedProfile();
        if (!profile || _draft.profiles.size() <= 1) return;
        const QString id = profile->id;
        for (int i = _draft.views.size() - 1; i >= 0; --i)
            if (_draft.views[i].profileId == id) _draft.views.remove(i);
        for (int i = 0; i < _draft.profiles.size(); ++i)
            if (_draft.profiles[i].id == id) {
                _draft.profiles.remove(i);
                break;
            }
        bool foundActive = false;
        for (const auto& view : _draft.views)
            foundActive |= view.id == _draft.activeViewId;
        if (!foundActive) _draft.activeViewId = _draft.views.first().id;
        rebuildProfiles();
    }
    void editSelectedProfile()
    {
        auto* profile = selectedProfile();
        if (!profile) return;
        const QString profileId = profile->id;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename profile"),
            tr("Profile name"), QLineEdit::Normal, profile->name, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        profile->name = name;

        QDialog dialog(this);
        dialog.setWindowTitle(tr("Servers connected in %1").arg(name));
        auto* layout = new QVBoxLayout(&dialog);
        auto* info = new QLabel(tr("Changing profile membership may connect or "
            "disconnect servers when that profile is active."), &dialog);
        info->setWordWrap(true);
        layout->addWidget(info);
        auto* list = new QListWidget(&dialog);
        layout->addWidget(list);
        for (const auto& account : _draft.accounts) {
            auto* item = new QListWidgetItem(account.name, list);
            item->setData(Qt::UserRole, account.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(profile->accountIds.contains(account.id)
                ? Qt::Checked : Qt::Unchecked);
        }
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            QStringList selected;
            for (int i = 0; i < list->count(); ++i) {
                auto* item = list->item(i);
                if (item->checkState() == Qt::Checked)
                    selected.append(item->data(Qt::UserRole).toString());
            }
            if (selected.isEmpty()) {
                QMessageBox::warning(this, tr("Empty profile"),
                                     tr("Select at least one server."));
                return;
            }
            profile->accountIds = selected;
            for (auto& view : _draft.views) {
                if (view.profileId != profileId) continue;
                for (int i = view.accountIds.size() - 1; i >= 0; --i)
                    if (!selected.contains(view.accountIds[i]))
                        view.accountIds.removeAt(i);
                if (view.accountIds.isEmpty())
                    view.accountIds.append(selected.first());
            }
        }
        rebuildProfiles(profileId);
    }
    void moveProfile(int offset)
    {
        auto* profile = selectedProfile();
        if (!profile) return;
        const QString id = profile->id;
        int index = -1;
        for (int i = 0; i < _draft.profiles.size(); ++i)
            if (_draft.profiles[i].id == id) { index = i; break; }
        const int next = index + offset;
        if (index < 0 || next < 0 || next >= _draft.profiles.size()) return;
        _draft.profiles.swapItemsAt(index, next);
        rebuildProfiles(id);
    }
    void moveView(int offset)
    {
        auto* view = selectedView();
        if (!view) return;
        const QString id = view->id;
        const QString profileId = view->profileId;
        int index = -1;
        for (int i = 0; i < _draft.views.size(); ++i)
            if (_draft.views[i].id == id) { index = i; break; }
        if (index < 0) return;
        const int direction = offset < 0 ? -1 : 1;
        int candidate = index + direction;
        while (candidate >= 0 && candidate < _draft.views.size()
               && _draft.views[candidate].profileId != profileId)
            candidate += direction;
        if (candidate < 0 || candidate >= _draft.views.size()) return;
        _draft.views.swapItemsAt(index, candidate);
        rebuildViews(id);
    }
    void moveMember(int offset)
    {
        auto* view = selectedView();
        auto* item = _members->currentItem();
        if (!view || !item || item->checkState() != Qt::Checked) return;
        const QString id = item->data(Qt::UserRole).toString();
        const int index = view->accountIds.indexOf(id);
        const int next = index + offset;
        if (index < 0 || next < 0 || next >= view->accountIds.size()) return;
        view->accountIds.swapItemsAt(index, next);
        rebuildMembers();
        for (int i = 0; i < _members->count(); ++i)
            if (_members->item(i)->data(Qt::UserRole).toString() == id) {
                _members->setCurrentRow(i);
                break;
            }
    }

    AccountManager& _manager;
    WorkspaceLayout _draft;
    QComboBox* _profiles = nullptr;
    QComboBox* _views = nullptr;
    QLineEdit* _viewName = nullptr;
    QListWidget* _members = nullptr;
};

} // namespace

ServersSidebarPage::ServersSidebarPage(AccountManager& accounts, QWidget* parent)
    : QWidget(parent), _accounts(accounts)
{
    setObjectName(QStringLiteral("serversSidebarPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 5, 4, 4);
    layout->setSpacing(5);

    // Mirror the Channels/Unread header toolbar: 34px total, 28px tools.
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("serversToolbar"));
    toolbar->setFixedHeight(34);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(3);
    // The tab already names this surface; keep its toolbar icon-only.
    toolbarLayout->addStretch();
    auto* add = new QToolButton(toolbar);
    add->setObjectName(QStringLiteral("serversAdd"));
    add->setAutoRaise(true);
    add->setIconSize(QSize(20, 20));
    add->setFixedSize(30, 28);
    add->setIcon(IconUtils::tintedSymbolicIcon(QStringLiteral(":/icons/add"),
                 palette().color(QPalette::ButtonText)));
    add->setToolTip(tr("Add server"));
    add->setAccessibleName(tr("Add server"));
    toolbarLayout->addWidget(add);
    auto* editLayout = new QToolButton(toolbar);
    editLayout->setObjectName(QStringLiteral("serversEditLayout"));
    editLayout->setAutoRaise(true);
    editLayout->setIconSize(QSize(20, 20));
    editLayout->setFixedSize(28, 28);
    editLayout->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/menu-settings"),
        palette().color(QPalette::ButtonText)));
    editLayout->setToolTip(tr("Edit layout"));
    editLayout->setAccessibleName(tr("Edit layout"));
    toolbarLayout->addWidget(editLayout);
    layout->addWidget(toolbar);

    _tree = new QTreeView(this);
    _tree->setObjectName(QStringLiteral("serversWorkspaceTree"));
    _tree->setModel(_accounts.workspaceTreeModel());
    _tree->setHeaderHidden(true);
    _tree->setFrameShape(QFrame::NoFrame);
    _tree->setMouseTracking(true);
    _tree->setItemDelegate(new ServerStatusDelegate(
        [this](const QModelIndex& index) {
            const QString id = index.data(
                WorkspaceTreeModel::AccountIdRole).toString();
            if (!id.isEmpty()) emit serverSettingsRequested(id);
        },
        [this](const QModelIndex& index) {
            const QString id = index.data(
                WorkspaceTreeModel::AccountIdRole).toString();
            if (id.isEmpty()) return;
            if (ServerStatusDelegate::connectionState(index)
                == AccountConnectionState::Connected)
                emit disconnectRequested(id);
            else
                emit retryRequested(id);
        }, _tree));
    _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setUniformRowHeights(true);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->expandAll();
    layout->addWidget(_tree, 1);

    connect(_tree, &QTreeView::doubleClicked, this,
            [this](const QModelIndex&) { openSelected(); });
    connect(_tree, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint& point) {
        const QModelIndex index = _tree->indexAt(point);
        if (!index.isValid()) return;
        _tree->setCurrentIndex(index);
        QMenu menu(this);
        menu.addAction(tr("Open"), this, &ServersSidebarPage::openSelected);
        if (index.data(WorkspaceTreeModel::NodeKindRole).toInt()
            == WorkspaceTreeModel::AccountNode) {
            menu.addAction(tr("Edit server…"), this,
                           &ServersSidebarPage::editSelectedAccount);
        }
        menu.addAction(tr("Edit layout…"), this,
                       &ServersSidebarPage::editLayout);
        menu.exec(_tree->viewport()->mapToGlobal(point));
    });
    connect(add, &QToolButton::clicked, this, &ServersSidebarPage::addServerRequested);
    connect(editLayout, &QToolButton::clicked, this, &ServersSidebarPage::editLayout);
    connect(_accounts.workspaceTreeModel(), &QAbstractItemModel::rowsInserted,
            this, [this] { _tree->expandAll(); });
    connect(&_accounts, &AccountManager::layoutChanged,
            this, [this] { _tree->expandAll(); });
}

void ServersSidebarPage::openSelected()
{
    const QModelIndex index = _tree->currentIndex();
    if (!index.isValid()) return;
    const int kind = index.data(WorkspaceTreeModel::NodeKindRole).toInt();
    if (kind == WorkspaceTreeModel::AccountNode) {
        const QString id = index.data(WorkspaceTreeModel::AccountIdRole).toString();
        if (!id.isEmpty()) emit accountOpenRequested(id);
    } else if (kind == WorkspaceTreeModel::ViewNode) {
        const QString id = index.data(WorkspaceTreeModel::ViewIdRole).toString();
        if (!id.isEmpty()) emit viewOpenRequested(id);
    }
}

void ServersSidebarPage::editSelectedAccount()
{
    const QString id = _tree->currentIndex().data(
        WorkspaceTreeModel::AccountIdRole).toString();
    if (!id.isEmpty())
        emit serverSettingsRequested(id);
}

void ServersSidebarPage::editLayout()
{
    WorkspaceLayoutDialog dialog(_accounts, this);
    dialog.exec();
}

} // namespace Mattermost
