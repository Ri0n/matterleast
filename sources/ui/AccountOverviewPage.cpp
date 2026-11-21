#include "AccountOverviewPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace Mattermost {

AccountOverviewPage::AccountOverviewPage(QWidget* parent)
    : QWidget(parent)
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

    _accounts = new QListWidget(this);
    _accounts->setObjectName(QStringLiteral("configuredAccountsList"));
    layout->addWidget(_accounts, 1);
    connect(_accounts, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
        if (item)
            emit accountSelected(item->data(Qt::UserRole).toString());
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
}

QString AccountOverviewPage::selectedAccountId() const
{
    const auto* item = _accounts->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void AccountOverviewPage::setEntries(const QVector<Entry>& entries,
                                     const QString& selectedId)
{
    const QSignalBlocker blocked(_accounts);
    _accounts->clear();
    int selection = -1;
    for (const Entry& account : entries) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2 — %3")
                .arg(account.display, account.server, account.status),
            _accounts);
        item->setData(Qt::UserRole, account.id);
        if (account.id == selectedId)
            selection = _accounts->count() - 1;
    }
    if (selection < 0 && !_accounts->count())
        selection = -1;
    else if (selection < 0)
        selection = 0;
    if (selection >= 0)
        _accounts->setCurrentRow(selection);
    _retry->setEnabled(_accounts->count() > 0);
    _signIn->setEnabled(_accounts->count() > 0);
}

void AccountOverviewPage::setError(const QString& message)
{
    _errorLabel->setText(message);
    _errorLabel->setVisible(!message.isEmpty());
}

} // namespace Mattermost
