#include "SessionVerificationRetry.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <utility>

namespace Mattermost {
namespace {
Q_LOGGING_CATEGORY(verificationLog, "mattermost.account.restore")

int retryDelayMs(int completedAttempts)
{
    // Only temporary transport failures are retried; bounded delays avoid
    // flooding an unavailable server or trapping users in a login loop.
    return completedAttempts == 1 ? 1500 : 4000;
}
} // namespace

SessionVerificationRetry::SessionVerificationRetry(QObject* parent)
    : QObject(parent)
{
}

void SessionVerificationRetry::start(Check check, Finished finished)
{
    if (_active || !check || !finished)
        return;
    _check = std::move(check);
    _finished = std::move(finished);
    _active = true;
    attempt();
}

void SessionVerificationRetry::cancel()
{
    _active = false;
    _inFlight = false;
    _check = {};
    _finished = {};
    deleteLater();
}

void SessionVerificationRetry::attempt()
{
    if (!_active || _inFlight)
        return;
    ++_attempts;
    _inFlight = true;
    const QPointer<SessionVerificationRetry> guard(this);
    _check([guard](bool authenticated, bool transient) {
        if (guard)
            guard->checked(authenticated, transient);
    });
}

void SessionVerificationRetry::checked(bool authenticated, bool transient)
{
    if (!_active || !_inFlight)
        return;
    _inFlight = false;
    if (!authenticated && transient && _attempts < MaxAttempts) {
        const int delay = retryDelayMs(_attempts);
        qCInfo(verificationLog)
            << "Saved session verification temporarily unavailable;"
            << "attempt" << _attempts << "of" << MaxAttempts
            << "retrying after" << delay << "ms";
        QTimer::singleShot(delay, this, [this] { attempt(); });
        return;
    }

    _active = false;
    _check = {};
    const auto finished = std::move(_finished);
    qCInfo(verificationLog)
        << "Saved session verification finished:"
        << (authenticated ? "authenticated"
                          : transient ? "network-unavailable" : "rejected")
        << "attempts" << _attempts;
    finished(authenticated, transient && !authenticated);
    deleteLater();
}

} // namespace Mattermost
