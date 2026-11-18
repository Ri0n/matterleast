#pragma once

#include <QList>
#include <QString>

namespace MatterLeast::PluginApi {

// Transport-independent status. All fields describe the local Calls session.
enum class CallsState {
    Idle,
    Connecting,
    Connected,
    Leaving,
    Failed
};

struct CallsParticipant {
    QString userId;
    QString sessionId;
    bool microphoneMuted = false;
};

struct CallsSnapshot {
    CallsState state = CallsState::Idle;
    QString channelId;
    QString callId;
    bool microphoneMuted = false;
    QList<CallsParticipant> participants;
    QString errorMessage;
};

} // namespace MatterLeast::PluginApi
