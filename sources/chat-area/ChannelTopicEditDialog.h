#pragma once

#include <QDialog>
#include <QPointer>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace Mattermost {

class Backend;
class BackendChannel;

// Edits only Mattermost's channel 'header' field (the visible topic). The
// server remains the sole authority for permission checks and successful state.
class ChannelTopicEditDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ChannelTopicEditDialog(Backend& backend, BackendChannel& channel,
                                    QWidget* parent = nullptr);

private:
    void refreshValidation();
    void save();

    Backend& _backend;
    QPointer<BackendChannel> _channel;
    QString _originalHeader;
    QPlainTextEdit* _editor = nullptr;
    QLabel* _counter = nullptr;
    QLabel* _error = nullptr;
    QPushButton* _save = nullptr;
    bool _saving = false;
};

} // namespace Mattermost
