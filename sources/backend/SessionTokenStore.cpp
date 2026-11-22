#include "SessionTokenStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QTimer>
#include <QUrl>

#ifdef MATTERLEAST_HAVE_KEYCHAIN
// Bundled QtKeychain exposes <qtkeychain/keychain.h>; distro development
// packages install <qt5keychain/keychain.h> or <qt6keychain/keychain.h>.
#if __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#elif QT_VERSION_MAJOR == 5
#include <qt5keychain/keychain.h>
#else
#include <qt6keychain/keychain.h>
#endif
#endif

namespace Mattermost {
namespace {
constexpr auto KeychainService = "io.github.Ri0n.MatterLeast";
#ifdef MATTERLEAST_HAVE_KEYCHAIN
// libsecret/QtKeychain can still deliver the completion after the requesting
// Account has been destroyed. A job must not be owned by that short-lived
// context: QtKeychain holds it until finished() and auto-deletes the job.
// The QObject connection's *receiver* remains the original context, so no
// callbacks are invoked for a destroyed account.
QObject* keychainJobParent()
{
    return QCoreApplication::instance();
}

void readKey(QObject* context, const QString& entryKey,
             SessionTokenStore::ReadCallback callback)
{
    auto* job = new QKeychain::ReadPasswordJob(QString::fromLatin1(KeychainService),
                                               keychainJobParent());
    job->setKey(entryKey);
    job->setInsecureFallback(false);
    // Log only a short digest prefix, never token, login or server URL.
    qInfo().noquote() << "MatterLeast keyring lookup: service" << KeychainService
                      << "key-prefix" << entryKey.left(27);
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [callback = std::move(callback)](QKeychain::Job* finished) {
        if (!callback) return;
        const auto* readJob = static_cast<QKeychain::ReadPasswordJob*>(finished);
        if (finished->error() == QKeychain::NoError) {
            const QString token = readJob->textData();
            callback(token.isEmpty() ? SessionTokenStore::ReadStatus::NotFound
                                     : SessionTokenStore::ReadStatus::Found, token);
        } else if (finished->error() == QKeychain::EntryNotFound) {
            callback(SessionTokenStore::ReadStatus::NotFound, {});
        } else {
            qWarning() << "Unable to read saved MatterLeast session from keyring:"
                       << static_cast<int>(finished->error());
            callback(SessionTokenStore::ReadStatus::Unavailable, {});
        }
    });
    job->start();
}

void removeKey(QObject* context, const QString& entryKey)
{
    auto* job = new QKeychain::DeletePasswordJob(QString::fromLatin1(KeychainService),
                                                 keychainJobParent());
    job->setKey(entryKey);
    job->setInsecureFallback(false);
    job->start();
}
#endif

QString legacyKeyFor(const QString& server, const QString& username)
{
    const QUrl url(server.trimmed());
    const QString oldOrigin = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment
                                           | QUrl::StripTrailingSlash)
                                    .toString(QUrl::FullyEncoded)
                                    .toLower();
    QByteArray material = oldOrigin.toUtf8();
    material.append(char(0));
    material.append(username.trimmed().toUtf8());
    return QStringLiteral("login-token-v1-")
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}
}

QString SessionTokenStore::keyFor(const QString& server,
                                  const QString& username)
{
    QUrl canonical(server.trimmed());
    canonical = canonical.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment
                                   | QUrl::StripTrailingSlash);
    // QUrl::StripTrailingSlash does not consistently remove the single root
    // slash. Our previous NetworkSessionContext always appended '/', changing
    // the SHA256 key for the same saved account. Canonicalize root explicitly.
    if (canonical.path() == QStringLiteral("/"))
        canonical.setPath(QString());
    return legacyKeyFor(canonical.toString(QUrl::FullyEncoded), username);
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
    const QString canonicalKey = keyFor(server, username);
    const QString oldKey = legacyKeyFor(server, username);
    readKey(context, canonicalKey,
        [context, canonicalKey, oldKey, callback = std::move(callback)](
            ReadStatus status, const QString& token) mutable {
        if (status == ReadStatus::NotFound && oldKey != canonicalKey) {
            // Compatibility with the transient broken root-slash format
            // produced by early multi-account builds. Do not delete it here:
            // only authenticated logout removes session credentials.
            qInfo("Canonical key not found; trying pre-normalization root key");
            readKey(context, oldKey, std::move(callback));
            return;
        }
        if (callback) callback(status, token);
    });
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
                                                keychainJobParent());
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
    const QString canonicalKey = keyFor(server, username);
    removeKey(context, canonicalKey);
    const QString oldKey = legacyKeyFor(server, username);
    if (oldKey != canonicalKey)
        removeKey(context, oldKey);
#else
    Q_UNUSED(context)
    Q_UNUSED(server)
    Q_UNUSED(username)
#endif
}

} // namespace Mattermost
