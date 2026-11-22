#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace Mattermost {

// One lightweight scheduler for all temporarily unreachable saved accounts.
// A network route change preempts backoff; if the OS misses a VPN transition,
// capped exponential retries eventually recover without user interaction.
// There is no Backend or secret retained while an account is waiting.
class TransientAccountReconnect final : public QObject
{
    Q_OBJECT
public:
    explicit TransientAccountReconnect(QObject* parent = nullptr);

    void schedule(const QString& accountId);
    void cancel(const QString& accountId);
    void resumeOnNetworkChange();
    bool isScheduled(const QString& accountId) const;
    int scheduledCount() const { return _deadlines.size(); }

signals:
    void retryDue(const QStringList& accountIds);

private:
    void arm();
    void deliverDue();

    QElapsedTimer _clock;
    QTimer _timer;
    QHash<QString, qint64> _deadlines;
    QHash<QString, int> _failedCycles;
};

} // namespace Mattermost
