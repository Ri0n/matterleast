#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QByteArray>
#include <QString>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTextBrowser;

namespace Mattermost {
class Backend;

class AgentThreadChatDialog final : public QDialog
{
    Q_OBJECT
public:
    AgentThreadChatDialog(Backend& backend, QString channelId,
                           QString rootId, QWidget* parent = nullptr);
    ~AgentThreadChatDialog() override;

private:
    void submit();
    void readOutput();
    void handleEvent(const QJsonObject& event);
    void updateAssistant(const QString& content);
    void finishTurn(int exitCode);
    void showSetupInstructions();
    QString makeInitialPrompt(const QString& message) const;
    QString executablePath() const;

    Backend& _backend;
    QString _channelId;
    QString _rootId;
    QString _token;
    QString _sessionId;
    QProcess* _agentProcess = nullptr;
    QTextBrowser* _messages = nullptr;
    QPlainTextEdit* _input = nullptr;
    QLabel* _status = nullptr;
    QCheckBox* _allowPosting = nullptr;
    QPushButton* _send = nullptr;
    QPushButton* _cancel = nullptr;
    QByteArray _outputBuffer;
    QString _streamedText;
    QString _resultText;
    bool _hasResult = false;
    bool _running = false;
    int _assistantStart = -1;
};
} // namespace Mattermost
