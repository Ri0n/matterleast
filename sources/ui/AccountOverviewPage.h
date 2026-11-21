#pragma once

#include <QVector>
#include <QWidget>

class QListWidget;
class QPushButton;
class QLabel;

namespace Mattermost {

// Presents already-configured accounts even when no Backend exists.
class AccountOverviewPage final : public QWidget
{
    Q_OBJECT
public:
    struct Entry {
        QString id;
        QString display;
        QString server;
        QString status;
    };

    explicit AccountOverviewPage(QWidget* parent = nullptr);
    void setEntries(const QVector<Entry>& entries, const QString& selectedId);
    void setError(const QString& message);

signals:
    void accountSelected(const QString& accountId);
    void retryRequested(const QString& accountId);
    void signInRequested(const QString& accountId);
    void addServerRequested();

private:
    QString selectedAccountId() const;

    QListWidget* _accounts = nullptr;
    QLabel* _errorLabel = nullptr;
    QPushButton* _retry = nullptr;
    QPushButton* _signIn = nullptr;
};

} // namespace Mattermost
