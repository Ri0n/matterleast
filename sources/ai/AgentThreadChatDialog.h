#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QMessageBox;

namespace Mattermost {
class AcpClient;
class Backend;

class AgentThreadChatDialog final : public QDialog
{
    Q_OBJECT
public:
    AgentThreadChatDialog(Backend& backend, QString channelId,
                          QString rootId, QWidget* parent = nullptr);
    ~AgentThreadChatDialog() override;

private:
    void startAgent(bool restoreSession = false);
    QString executablePath() const;
    QString initialPrompt(const QString& message) const;
    void submit();
    void appendAssistantChunk(const QString& text);
    void replaceAssistant(const QString& text);
    void finishTurn(const QString& reason);
    void requestPermission(const QJsonValue& id, const QJsonObject& params);
    void closePermissionDialogs();
    void updateControls();

    Backend& _backend;
    QString _channelId;
    QString _rootId;
    QString _token;
    QString _savedSessionId;
    AcpClient* _client = nullptr;
    QTextBrowser* _messages = nullptr;
    QPlainTextEdit* _input = nullptr;
    QLabel* _status = nullptr;
    QCheckBox* _allowPosting = nullptr;
    QPushButton* _send = nullptr;
    QPushButton* _cancel = nullptr;
    QPushButton* _restart = nullptr;
    QList<QPointer<QMessageBox>> _permissionDialogs;
    QString _streamedText;
    bool _running = false;
    bool _firstTurn = true;
    int _assistantStart = -1;
};

} // namespace Mattermost
