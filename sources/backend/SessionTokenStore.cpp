#include "SessionTokenStore.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QTimer>
#include <QUrl>

#ifdef MATTERLEAST_HAVE_KEYCHAIN
#include <qtkeychain/keychain.h>
#endif

namespace Mattermost {
namespace {
constexpr auto KeychainService = "io.github.Ri0n.MatterLeast";
}

QString SessionTokenStore::keyFor(const QString& server,
                                  const QString& username)
{
    const QUrl url(server.trimmed());
    const QString origin = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery
                                        | QUrl::RemoveFragment
                                        | QUrl::StripTrailingSlash)
                               .toString(QUrl::FullyEncoded)
                               .toLower();
    QByteArray material = origin.toUtf8();
    material.append(char(0));
    material.append(username.trimmed().toUtf8());
    return QStringLiteral("login-token-v1-")
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

void SessionTokenStore::read(QObject* context, const QString& server,
                             const QString& username, ReadCallback callback)
{
    if (!context || server.isEmpty() || username.isEmpty()) {
        if (context) {
            QTimer::singleShot(0, context, [callback = std::move(callback)] {
                if (callback) callback(ReadStatus::NotFound, {});
            });
        }
        return;
    }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QString::fromLatin1(KeychainService),
                                               context);
    job->setKey(keyFor(server, username));
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [callback = std::move(callback)](QKeychain::Job* finished) {
        if (!callback) return;
        const auto* readJob = static_cast<QKeychain::ReadPasswordJob*>(finished);
        if (finished->error() == QKeychain::NoError) {
            const QString token = readJob->textData();
            callback(token.isEmpty() ? ReadStatus::NotFound : ReadStatus::Found,
                     token);
        } else if (finished->error() == QKeychain::EntryNotFound) {
            callback(ReadStatus::NotFound, {});
        } else {
            qWarning() << "Unable to read saved MatterLeast session from keyring:"
                       << static_cast<int>(finished->error());
            callback(ReadStatus::Unavailable, {});
        }
    });
    job->start();
#else
    QTimer::singleShot(0, context, [callback = std::move(callback)] {
        if (callback) callback(ReadStatus::Unavailable, {});
    });
#endif
}

void SessionTokenStore::write(QObject* context, const QString& server,
                              const QString& username, const QString& token,
                              WriteCallback callback)
{
    if (!context || server.isEmpty() || username.isEmpty() || token.isEmpty()) {
        if (context && callback) {
            QTimer::singleShot(0, context, [callback = std::move(callback)] {
                callback(false);
            });
        }
        return;
    }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(QString::fromLatin1(KeychainService),
                                                context);
    job->setKey(keyFor(server, username));
    job->setTextData(token);
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [callback = std::move(callback)](QKeychain::Job* finished) {
        const bool success = finished->error() == QKeychain::NoError;
        if (!success) {
            qWarning() << "Could not persist MatterLeast session to keyring:"
                       << static_cast<int>(finished->error());
        }
        if (callback) callback(success);
    });
    job->start();
#else
    QTimer::singleShot(0, context, [callback = std::move(callback)] {
        if (callback) callback(false);
    });
#endif
}

void SessionTokenStore::remove(QObject* context, const QString& server,
                               const QString& username)
{
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    if (!context || server.isEmpty() || username.isEmpty()) return;
    auto* job = new QKeychain::DeletePasswordJob(QString::fromLatin1(KeychainService),
                                                 context);
    job->setKey(keyFor(server, username));
    job->setInsecureFallback(false);
    job->start();
#else
    Q_UNUSED(context)
    Q_UNUSED(server)
    Q_UNUSED(username)
#endif
}

} // namespace Mattermost
