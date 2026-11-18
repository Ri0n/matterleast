#pragma once

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QResizeEvent;

namespace Mattermost {

class Backend;
class BackendPost;
class BackendUser;

// Owns a snapshot of the original post; the chat row may be virtualized away
// while the dialog is open or while the REST request is in flight.
class ForwardPostDialog final : public QDialog
{
public:
    explicit ForwardPostDialog(Backend& backend,
                               const BackendPost& post,
                               QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void populateDestinations();
    void filterDestinations(const QString& text);
    void updateSubmitEnabled();
    void resolveVisibleDirectNames();
    void updateResolvedUser(const QString& userId);
    void searchRemoteUsers();
    void showRemoteUsers(const QVector<const BackendUser*>& users);
    void removeRemoteResults();
    void submit();
    void deliverToDestination(const QString& channelId, const QString& userId,
                              bool newDirect);
    void sendToChannel(const QString& channelId);
    void showFailure(const QString& message);

    Backend& backend_;
    QString sourceChannelId_;
    QString sourcePostId_;
    QString sourceTeamSlug_;
    QString sourceTeamId_;
    QString sourceMessage_;
    QString sourceAuthorId_;
    QString permalink_;
    bool sourceHasFileAttachments_ = false;
    bool usePermalink_ = true;
    int sourceType_ = 0;
    bool submitting_ = false;

    // Stable pointers to locally materialized DM rows. Only their text changes
    // when a profile becomes known: no re-sorting, selection drift or model reset.
    QHash<QString, QListWidgetItem*> directRows_;
    QSet<QString> requestedProfiles_;
    QSet<QString> inFlightProfiles_;
    QVector<QListWidgetItem*> remoteRows_;
    QTimer searchTimer_;
    int searchGeneration_ = 0;

    QLineEdit* search_ = nullptr;
    QListWidget* destinations_ = nullptr;
    QPlainTextEdit* comment_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* forwardButton_ = nullptr;
};

} // namespace Mattermost
