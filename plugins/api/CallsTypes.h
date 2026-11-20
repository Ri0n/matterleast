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
    // The server starts each newly joined session muted.
    bool microphoneMuted = true;
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
