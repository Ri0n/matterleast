#pragma once

namespace Mattermost {

// Transport/authentication state belongs to an account, not any QWidget.
enum class AccountConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Unavailable,
    AuthenticationRequired,
    SetupRequired
};

} // namespace Mattermost
