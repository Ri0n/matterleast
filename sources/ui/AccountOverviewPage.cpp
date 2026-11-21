#include "AccountOverviewPage.h"

#include <QFont>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "workspaces/AccountListModel.h"

namespace Mattermost {

AccountOverviewPage::AccountOverviewPage(AccountListModel& model,
                                         QWidget* parent)
    : QWidget(parent)
    , _model(model)
{
    setObjectName(QStringLiteral("accountOverviewPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 18, 24, 18);

    auto* heading = new QLabel(tr("Servers"), this);
    QFont font = heading->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);

    auto* description = new QLabel(
        tr("Your accounts remain available here when a server is offline."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    _errorLabel = new QLabel(this);
    _errorLabel->setWordWrap(true);
    _errorLabel->hide();
    layout->addWidget(_errorLabel);

    _accounts = new QListView(this);
    _accounts->setObjectName(QStringLiteral("configuredAccountsList"));
    _accounts->setModel(&_model);
    layout->addWidget(_accounts, 1);
    connect(_accounts, &QListView::clicked, this,
            [this](const QModelIndex& index) {
        const QString id = _model.idAt(index.row());
        if (!id.isEmpty())
            emit accountSelected(id);
    });

    auto* buttons = new QHBoxLayout;
    _retry = new QPushButton(tr("Connect"), this);
    _signIn = new QPushButton(tr("Sign in…"), this);
    auto* add = new QPushButton(tr("Add server…"), this);
    buttons->addWidget(_retry);
    buttons->addWidget(_signIn);
    buttons->addStretch(1);
    buttons->addWidget(add);
    layout->addLayout(buttons);

    connect(_retry, &QPushButton::clicked, this, [this] {
        const QString id = selectedAccountId();
        if (!id.isEmpty())
            emit retryRequested(id);
    });
    connect(_signIn, &QPushButton::clicked, this, [this] {
        const QString id = selectedAccountId();
        if (!id.isEmpty())
            emit signInRequested(id);
    });
    connect(add, &QPushButton::clicked, this,
            &AccountOverviewPage::addServerRequested);

    connect(&_model, &AccountListModel::selectedAccountChanged, this,
            [this](const QString&) { syncSelection(); });
    connect(&_model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int, int) {
        syncSelection();
        syncButtons();
    });
    connect(&_model, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex&, int, int) {
        syncSelection();
        syncButtons();
    });
    syncSelection();
    syncButtons();
}

QString AccountOverviewPage::selectedAccountId() const
{
    return _model.idAt(_accounts->currentIndex().row());
}

void AccountOverviewPage::syncSelection()
{
    const int row = _model.rowForId(_model.selectedAccountId());
    const QModelIndex target = row < 0 ? QModelIndex() : _model.index(row, 0);
    if (_accounts->currentIndex() == target)
        return;
    const QSignalBlocker blocker(_accounts->selectionModel());
    _accounts->setCurrentIndex(target);
}

void AccountOverviewPage::syncButtons()
{
    const bool hasAccounts = _model.rowCount() > 0;
    _retry->setEnabled(hasAccounts);
    _signIn->setEnabled(hasAccounts);
}

void AccountOverviewPage::setError(const QString& message)
{
    _errorLabel->setText(message);
    _errorLabel->setVisible(!message.isEmpty());
}

} // namespace Mattermost
