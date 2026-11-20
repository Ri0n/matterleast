// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <functional>

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "plugins/api/CallsTypes.h"
#include "CallsMediaDevice.h"

class QCloseEvent;
class QEvent;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;

// Standalone, UI-only audio call window. The presentation follows Psi's
// stage-and-controls UX, but is independently implemented under LGPL.
// Media devices, transport, and GStreamer stay entirely outside this view.
class CallsWindow final : public QDialog
{
public:
    using MuteHandler = std::function<bool(bool muted)>;
    using EndHandler = std::function<void()>;
    using DeviceSelectionHandler = std::function<void(const QString& id)>;
    using RefreshDevicesHandler = std::function<void()>;

    explicit CallsWindow(MuteHandler mute, EndHandler end,
                         QString localUserId = {}, QWidget* parent = nullptr);

    void showForCall(const MatterLeast::PluginApi::CallsSnapshot& state,
                     const QString& conversationTitle);
    void updateSnapshot(const MatterLeast::PluginApi::CallsSnapshot& state);
    void setMicrophoneDeviceHandlers(DeviceSelectionHandler choose,
                                     RefreshDevicesHandler refresh);
    void setMicrophoneDevices(const QList<CallsMediaDevice>& available,
                              const QString& selectedId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct ParticipantTile {
        QFrame* frame = nullptr;
        QLabel* avatar = nullptr;
        QLabel* title = nullptr;
        QLabel* microphone = nullptr;
    };

    void updateParticipants(const QList<MatterLeast::PluginApi::CallsParticipant>& participants);
    void relayoutTiles();
    void updateHeader();
    void updateControls();
    void updateMicrophoneIcon();
    void rebuildMicrophoneDeviceMenu();
    void updateAppearance();
    void updateTimerLabel();
    static QString participantLabel(const QString& userId);

    MuteHandler mute_;
    EndHandler end_;
    DeviceSelectionHandler chooseMicrophone_;
    RefreshDevicesHandler refreshDevices_;
    QList<CallsMediaDevice> availableMicrophones_;
    QString selectedMicrophoneId_;
    MatterLeast::PluginApi::CallsSnapshot snapshot_;
    QString conversationTitle_;
    QString localUserId_;
    QStringList tileOrder_;
    QStringList laidOutOrder_;
    int tileColumns_ = 0;
    QGridLayout* grid_ = nullptr;
    QScrollArea* participantsScroll_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QLabel* callTitle_ = nullptr;
    QLabel* callSubtitle_ = nullptr;
    QLabel* participantCount_ = nullptr;
    QToolButton* microphone_ = nullptr;
    QToolButton* microphoneDevices_ = nullptr;
    QPushButton* endButton_ = nullptr;
    QTimer elapsedTick_;
    QElapsedTimer elapsedTime_;
    bool connectedOnce_ = false;
    bool updatingAppearance_ = false;
    QHash<QString, ParticipantTile> tiles_;
};
