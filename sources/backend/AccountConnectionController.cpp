#include "AccountConnectionController.h"

#include <QLoggingCategory>
#include <QtGlobal>
#include <limits>

#if defined(Q_OS_LINUX)
#include <QSocketNotifier>
#include <cerrno>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
#include <QNetworkInformation>
#elif QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_BEARERMANAGEMENT) && !defined(Q_OS_LINUX)
#include <QNetworkConfigurationManager>
#endif

namespace Mattermost {
namespace {
Q_LOGGING_CATEGORY(connectionLog, "mattermost.account.connection")

int retryDelayMs(int failures)
{
    // One backoff policy for saved-token verification and live WebSocket.
    // Network changes preempt the wait; the two-minute cap also guarantees
    // periodic recovery when a VPN route event is unavailable.
    if (failures == 1) return 1500;
    if (failures == 2) return 4000;
    const int exponent = qBound(0, failures - 3, 3);
    return qMin(120000, 15000 * (1 << exponent));
}
} // namespace

AccountConnectionController::AccountConnectionController(QObject* parent)
    : QObject(parent)
{
    _clock.start();
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &AccountConnectionController::deliverDue);

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    if (QNetworkInformation::instance() || QNetworkInformation::loadDefaultBackend()) {
        if (auto* info = QNetworkInformation::instance()) {
            connect(info, &QNetworkInformation::reachabilityChanged,
                    this, [this] { resumeOnNetworkChange(); });
            connect(info, &QNetworkInformation::transportMediumChanged,
                    this, [this] { resumeOnNetworkChange(); });
        }
    }
#elif QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_BEARERMANAGEMENT) && !defined(Q_OS_LINUX)
    auto* network = new QNetworkConfigurationManager(this);
    connect(network, &QNetworkConfigurationManager::onlineStateChanged,
            this, [this](bool) { resumeOnNetworkChange(); });
    connect(network, &QNetworkConfigurationManager::configurationChanged,
            this, [this](const QNetworkConfiguration&) { resumeOnNetworkChange(); });
#endif

#ifdef Q_OS_LINUX
    // VPN interface/route changes can occur without global reachability
    // changing. Use kernel notifications, not another polling timer.
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                            NETLINK_ROUTE);
    if (fd >= 0) {
        sockaddr_nl address {};
        address.nl_family = AF_NETLINK;
        address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR
            | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0) {
            _routeChangeSocket = fd;
            auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
            connect(notifier, &QSocketNotifier::activated, this, [this, fd] {
                char buffer[8192];
                bool changed = false;
                for (;;) {
                    const auto received = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                    if (received > 0) {
                        changed = true;
                        continue;
                    }
                    if (received < 0 && errno == EINTR)
                        continue;
                    break;
                }
                if (changed)
                    resumeOnNetworkChange();
            });
        } else {
            ::close(fd);
        }
    }
#endif
}

AccountConnectionController::~AccountConnectionController()
{
#ifdef Q_OS_LINUX
    if (_routeChangeSocket >= 0)
        ::close(_routeChangeSocket);
#endif
}

void AccountConnectionController::startSavedSession(const QString& accountId)
{
    if (accountId.isEmpty())
        return;
    attemptStarted(accountId);
    emit savedSessionDue(QStringList{accountId});
}

void AccountConnectionController::savedSessionFailed(const QString& accountId)
{
    schedule(accountId, Kind::SavedSession);
}

void AccountConnectionController::websocketUnavailable(const QString& accountId)
{
    if (!_pending.contains(accountId))
        schedule(accountId, Kind::WebSocket);
}

void AccountConnectionController::attemptStarted(const QString& accountId)
{
    _pending.remove(accountId);
    arm();
}

void AccountConnectionController::streamVerified(
    const QString& accountId, bool isReconnect, bool continuityVerified)
{
    cancel(accountId);
    if (isReconnect)
        emit replayResolved(accountId, continuityVerified);
}

void AccountConnectionController::cancel(const QString& accountId)
{
    _pending.remove(accountId);
    _failedCycles.remove(accountId);
    arm();
}

void AccountConnectionController::schedule(const QString& accountId, Kind kind)
{
    if (accountId.isEmpty())
        return;
    const int failures = _failedCycles.value(accountId, 0) + 1;
    _failedCycles.insert(accountId, failures);
    const int delay = retryDelayMs(failures);
    _pending.insert(accountId, Pending{kind, _clock.elapsed() + delay});
    qCInfo(connectionLog) << "Scheduling account connection retry"
                          << accountId << "phase" << int(kind) << "after" << delay << "ms";
    arm();
}

bool AccountConnectionController::isScheduled(const QString& accountId) const
{
    return _pending.contains(accountId);
}

void AccountConnectionController::resumeOnNetworkChange()
{
    // HTTP transports may need rotation even when a live WebSocket survives
    // the route change. The owner also probes such apparently live sockets.
    emit networkChanged();
    if (_pending.isEmpty())
        return;
    QStringList saved, sockets;
    for (auto it = _pending.cbegin(); it != _pending.cend(); ++it) {
        (it.value().kind == Kind::SavedSession ? saved : sockets).append(it.key());
    }
    _pending.clear();
    _timer.stop();
    if (!saved.isEmpty())
        emit savedSessionDue(saved);
    if (!sockets.isEmpty())
        emit webSocketDue(sockets);
}

void AccountConnectionController::arm()
{
    _timer.stop();
    if (_pending.isEmpty())
        return;
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (auto it = _pending.cbegin(); it != _pending.cend(); ++it)
        earliest = qMin(earliest, it.value().deadline);
    const qint64 remaining = qMax(qint64(0), earliest - _clock.elapsed());
    _timer.start(int(qMin(remaining, qint64(std::numeric_limits<int>::max()))));
}

void AccountConnectionController::deliverDue()
{
    const qint64 now = _clock.elapsed();
    QStringList saved, sockets;
    for (auto it = _pending.begin(); it != _pending.end();) {
        if (it.value().deadline <= now) {
            (it.value().kind == Kind::SavedSession ? saved : sockets).append(it.key());
            it = _pending.erase(it);
        } else {
            ++it;
        }
    }
    arm();
    if (!saved.isEmpty())
        emit savedSessionDue(saved);
    if (!sockets.isEmpty())
        emit webSocketDue(sockets);
}

} // namespace Mattermost
