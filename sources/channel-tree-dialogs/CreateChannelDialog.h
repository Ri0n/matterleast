#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QTextEdit;

namespace Mattermost {

class BackendTeam;

class CreateChannelDialog final : public QDialog
{
public:
    explicit CreateChannelDialog(const BackendTeam& team,
                                 QWidget* parent = nullptr);

    QString channelName() const;
    QString displayName() const;
    QString purpose() const;
    bool isPrivateChannel() const;

public slots:
    void accept() override;

private:
    static QString generatedChannelName(const QString& displayName);
    void updateGeneratedName();

    QLineEdit* displayNameEdit = nullptr;
    QLineEdit* channelNameEdit = nullptr;
    QTextEdit* purposeEdit = nullptr;
    QComboBox* typeCombo = nullptr;
    bool channelNameEdited = false;
};

} // namespace Mattermost
