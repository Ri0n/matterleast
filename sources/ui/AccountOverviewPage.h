#pragma once

#include <QWidget>

class QListView;
class QPushButton;
class QLabel;

namespace Mattermost {

class AccountListModel;

// Qt Widgets rendering of the QtCore account projection. Future QML screens
// can bind to the same model without depending on this widget at all.
class AccountOverviewPage final : public QWidget
{
    Q_OBJECT
public:
    explicit AccountOverviewPage(AccountListModel& model,
                                 QWidget* parent = nullptr);
    void setError(const QString& message);

signals:
    void accountSelected(const QString& accountId);
    void retryRequested(const QString& accountId);
    void signInRequested(const QString& accountId);
    void addServerRequested();
    void manageServersRequested();

private:
    QString selectedAccountId() const;
    void syncSelection();
    void syncButtons();

    AccountListModel& _model;
    QListView* _accounts = nullptr;
    QLabel* _errorLabel = nullptr;
    QPushButton* _retry = nullptr;
    QPushButton* _signIn = nullptr;
};

} // namespace Mattermost
