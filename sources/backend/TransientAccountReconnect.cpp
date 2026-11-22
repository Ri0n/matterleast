#include "TransientAccountReconnect.h"

#include <QLoggingCategory>
#include <QtGlobal>
#include <limits>

namespace Mattermost {
namespace {
Q_LOGGING_CATEGORY(reconnectLog, "mattermost.account.restore")

int retryDelayMs(int failures)
{
    // A single retry policy covers both initial verification and prolonged
    // VPN outages: no nested retry loops and no per-account timer/backend
    // waiting in memory. Later attempts cap at two minutes.
    if (failures == 1) return 1500;
    if (failures == 2) return 4000;
    const int exponent = qBound(0, failures - 3, 3);
    return qMin(120000, 15000 * (1 << exponent));
}
} // namespace

TransientAccountReconnect::TransientAccountReconnect(QObject* parent)
    : QObject(parent)
{
    _clock.start();
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &TransientAccountReconnect::deliverDue);
}

void TransientAccountReconnect::schedule(const QString& accountId)
{
    if (accountId.isEmpty()) return;
    const int failures = _failedCycles.value(accountId, 0) + 1;
    _failedCycles.insert(accountId, failures);
    const int delay = retryDelayMs(failures);
    _deadlines.insert(accountId, _clock.elapsed() + delay);
    qCInfo(reconnectLog) << "Saved account remains offline; scheduling retry"
                         << accountId << "after" << delay << "ms";
    arm();
}

void TransientAccountReconnect::attemptStarted(const QString& accountId)
{
    _deadlines.remove(accountId);
    arm();
}

void TransientAccountReconnect::cancel(const QString& accountId)
{
    _deadlines.remove(accountId);
    _failedCycles.remove(accountId);
    arm();
}

bool TransientAccountReconnect::isScheduled(const QString& accountId) const
{
    return _deadlines.contains(accountId);
}

void TransientAccountReconnect::resumeOnNetworkChange()
{
    if (_deadlines.isEmpty()) return;
    const QStringList ids = _deadlines.keys();
    _deadlines.clear();
    _timer.stop();
    qCInfo(reconnectLog) << "Network changed; immediately retrying"
                         << ids.size() << "unavailable accounts";
    emit retryDue(ids);
}

void TransientAccountReconnect::arm()
{
    _timer.stop();
    if (_deadlines.isEmpty())
        return;
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (auto it = _deadlines.cbegin(); it != _deadlines.cend(); ++it)
        earliest = qMin(earliest, it.value());
    const qint64 remaining = qMax(qint64(0), earliest - _clock.elapsed());
    _timer.start(int(qMin(remaining, qint64(std::numeric_limits<int>::max()))));
}

void TransientAccountReconnect::deliverDue()
{
    const qint64 now = _clock.elapsed();
    QStringList due;
    for (auto it = _deadlines.begin(); it != _deadlines.end();) {
        if (it.value() <= now) {
            due.append(it.key());
            it = _deadlines.erase(it);
        } else {
            ++it;
        }
    }
    arm();
    if (!due.isEmpty())
        emit retryDue(due);
}

} // namespace Mattermost
