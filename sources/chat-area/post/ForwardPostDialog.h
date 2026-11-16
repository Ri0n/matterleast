#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace Mattermost {

class Backend;
class BackendPost;

// Owns a snapshot of the original post; the chat row may be virtualized away
// while the dialog is open or while the REST request is in flight.
class ForwardPostDialog final : public QDialog
{
public:
    explicit ForwardPostDialog(Backend& backend,
                               const BackendPost& post,
                               QWidget* parent = nullptr);

private:
    void populateDestinations();
    void filterDestinations(const QString& text);
    void updateSubmitEnabled();
    void submit();

    Backend& backend_;
    QString sourceChannelId_;
    QString permalink_;
    int sourceType_ = 0;
    bool submitting_ = false;

    QLineEdit* search_ = nullptr;
    QListWidget* destinations_ = nullptr;
    QPlainTextEdit* comment_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* forwardButton_ = nullptr;
};

} // namespace Mattermost
