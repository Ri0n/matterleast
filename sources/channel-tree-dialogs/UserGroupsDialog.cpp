#include "UserGroupsDialog.h"

#include <algorithm>

#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "UserGroupEditorDialog.h"
#include "backend/Backend.h"

namespace Mattermost {
namespace {

constexpr int SearchDelayMs = 250;
constexpr int GroupSearchLimit = 100;

} // namespace

UserGroupsDialog::UserGroupsDialog(Backend& sourceBackend, QWidget* parent)
    : QDialog(parent)
    , backend(sourceBackend)
    , groupService(MentionGroupService::instance(sourceBackend))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("User groups"));
    resize(620, 620);

    auto* layout = new QVBoxLayout(this);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("User groups"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() > 0.0
        ? titleFont.pointSizeF() + 2.0 : titleFont.pointSizeF());
    title->setFont(titleFont);

    auto* createButton = new QPushButton(tr("Create group"), this);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(createButton);
    layout->addLayout(header);

    searchEdit = new QLineEdit(this);
    searchEdit->setPlaceholderText(tr("Search groups"));
    searchEdit->setClearButtonEnabled(true);
    layout->addWidget(searchEdit);

    table = new QTableWidget(this);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels(
        {tr("Group"), tr("Mention"), tr("Members")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    layout->addWidget(table, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    openButton = buttons->addButton(tr("Open"), QDialogButtonBox::ActionRole);
    openButton->setEnabled(false);
    layout->addWidget(buttons);

    searchTimer.setSingleShot(true);
    searchTimer.setInterval(SearchDelayMs);

    connect(&searchTimer, &QTimer::timeout,
            this, &UserGroupsDialog::reload);
    connect(searchEdit, &QLineEdit::textChanged,
            this, &UserGroupsDialog::scheduleSearch);
    connect(createButton, &QPushButton::clicked,
            this, &UserGroupsDialog::createGroup);
    connect(openButton, &QPushButton::clicked,
            this, &UserGroupsDialog::openSelectedGroup);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    connect(table, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) { openGroup(row); });
    connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
        openButton->setEnabled(table->currentRow() >= 0);
    });

    reload();
}

void UserGroupsDialog::scheduleSearch(const QString& text)
{
    searchTimer.stop();
    query = text.trimmed();
    ++generation;
    searchTimer.start();
}

void UserGroupsDialog::reload()
{
    const QString requestedQuery = query;
    const int requestedGeneration = generation;
    QPointer<UserGroupsDialog> guard(this);

    groupService.searchGroups(
        requestedQuery, GroupSearchLimit,
        [guard, requestedQuery, requestedGeneration](
            QVector<MentionGroup> groups) mutable {
            if (!guard
                || requestedGeneration != guard->generation
                || requestedQuery != guard->query) {
                return;
            }
            guard->showGroups(std::move(groups));
        });
}

void UserGroupsDialog::showGroups(QVector<MentionGroup> groups)
{
    std::sort(groups.begin(), groups.end(),
              [](const MentionGroup& lhs, const MentionGroup& rhs) {
        const QString left = lhs.displayName.isEmpty()
            ? lhs.name : lhs.displayName;
        const QString right = rhs.displayName.isEmpty()
            ? rhs.name : rhs.displayName;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    visibleGroups = std::move(groups);
    table->clearContents();
    table->setRowCount(visibleGroups.size());

    for (int row = 0; row < visibleGroups.size(); ++row) {
        const MentionGroup& group = visibleGroups.at(row);
        QString display = group.displayName.isEmpty()
            ? group.name : group.displayName;
        if (group.isArchived()) {
            display += tr(" (archived)");
        }

        auto* displayItem = new QTableWidgetItem(display);
        displayItem->setData(Qt::UserRole, group.id);
        if (!group.isCustom()) {
            displayItem->setToolTip(
                tr("Directory-backed group; details are read-only"));
        }

        table->setItem(row, 0, displayItem);
        table->setItem(
            row, 1,
            new QTableWidgetItem(
                group.name.isEmpty()
                    ? QString()
                    : QStringLiteral("@") + group.name));
        table->setItem(
            row, 2,
            new QTableWidgetItem(QString::number(group.memberCount)));
    }

    if (!visibleGroups.isEmpty()) {
        table->selectRow(0);
    } else {
        openButton->setEnabled(false);
    }
}

void UserGroupsDialog::openSelectedGroup()
{
    openGroup(table->currentRow());
}

void UserGroupsDialog::openGroup(int row)
{
    if (row < 0 || row >= visibleGroups.size()) {
        return;
    }

    auto* editor = new UserGroupEditorDialog(
        backend, visibleGroups.at(row), this);
    connect(editor, &QDialog::accepted, this, [this] {
        ++generation;
        reload();
    });
    editor->show();
}

void UserGroupsDialog::createGroup()
{
    auto* editor = new UserGroupEditorDialog(
        backend, std::nullopt, this);
    connect(editor, &QDialog::accepted, this, [this] {
        ++generation;
        reload();
    });
    editor->show();
}

} // namespace Mattermost
