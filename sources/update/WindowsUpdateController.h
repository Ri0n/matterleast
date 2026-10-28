#pragma once

// Q_OS_WIN is defined by QtGlobal, not by the compiler. Include it *before*
// testing the platform macro so AUTMOC and the implementation see the class.
#include <QtGlobal>
#ifdef Q_OS_WIN

#include <QObject>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkReply;

namespace Mattermost {
class WindowsUpdateController final : public QObject
{
    Q_OBJECT
public:
    enum class State { Unavailable, Idle, Checking, Downloading, Preparing, Ready, Failed };
    Q_ENUM(State)

    explicit WindowsUpdateController(QObject* parent = nullptr);
    ~WindowsUpdateController() override;

    State state() const { return _state; }
    QString availableVersion() const { return _identity; }
    QString lastError() const { return _error; }
    bool canApply() const { return _state == State::Ready; }
    bool manualCheckActive() const { return !_automatic; }
    bool isBusy() const
    {
        return _state == State::Checking || _state == State::Downloading
            || _state == State::Preparing;
    }
    QString checkChannel() const;
    QDateTime lastCheckTime() const;
    QString lastCheckResult() const;
    qint64 downloadedBytes() const { return _received; }
    qint64 downloadTotalBytes() const { return _size; }

public slots:
    void checkNow();
    void startUpdateAndRestart();

signals:
    void stateChanged(State state);
    void progressChanged(qint64 received, qint64 total);
    void updatePrepared(const QString& identity);
    void errorOccurred(const QString& description);
    void manualCheckStarted();
    void noUpdateAvailable(const QString& description);
    void checkHistoryChanged();

private:
    void check(bool automatic);
    void downloadBytes(const QUrl& url, int limit,
                       const std::function<void(QByteArray)>& done);
    void manifestReceived(const QByteArray& raw);
    void authenticatedManifest(const QByteArray& raw);
    void beginPackageDownload();
    void startPreparing(const QString& verifiedPackagePath);
    void restorePrepared();
    void setState(State state, const QString& error = {});
    void recordCheckResult(const QString& result);
    QString currentIdentity() const;
    bool installedLayout() const;

    QNetworkAccessManager _network;
    QTimer _periodic;
    QPointer<QNetworkReply> _reply;
    QFile _packageFile;
    QCryptographicHash _digest { QCryptographicHash::Sha256 };
    QByteArray _rawManifest;
    QString _root;
    QString _checkChannel;
    QString _identity;
    QString _sha256;
    QUrl _packageUrl;
    qint64 _size = 0;
    qint64 _received = 0;
    State _state = State::Unavailable;
    QString _error;
    bool _automatic = true;
};
} // namespace Mattermost
#endif // Q_OS_WIN
