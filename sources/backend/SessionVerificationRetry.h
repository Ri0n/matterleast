#pragma once

#include <functional>

#include <QObject>

// Bounded verification of an already saved server session. The caller owns the
// Backend and its credentials; this helper controls only when /users/me is
// retried after temporary transport failures. A cancelled/destroyed QObject
// cannot deliver a stale authentication result.
namespace Mattermost {

class SessionVerificationRetry final : public QObject
{
    Q_OBJECT
public:
    using CheckDone = std::function<void(bool authenticated, bool transient)>;
    using Check = std::function<void(CheckDone)>;
    using Finished = std::function<void(bool authenticated, bool transient)>;

    explicit SessionVerificationRetry(QObject* parent = nullptr);
    void start(Check check, Finished finished);
    void cancel();

    static constexpr int MaxAttempts = 3;

private:
    void attempt();
    void checked(bool authenticated, bool transient);

    Check _check;
    Finished _finished;
    int _attempts = 0;
    bool _active = false;
    bool _inFlight = false;
};

} // namespace Mattermost
