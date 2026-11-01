#include "HermesCredentialStore.h"

#include <QObject>

#ifdef MATTERLEAST_HAVE_KEYCHAIN
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qt5keychain/keychain.h>)
#include <qt5keychain/keychain.h>
#else
#include <keychain.h>
#endif
#endif

namespace Mattermost {
namespace HermesCredentialStore {
namespace {
const QString Service = QStringLiteral("MatterLeast Hermes AI");

QString entry(const QString& variable)
{
    return QStringLiteral("provider/") + variable;
}
} // namespace

bool available()
{
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

bool supportedProvider(const QString& variable)
{
    return variable == QStringLiteral("OPENROUTER_API_KEY")
        || variable == QStringLiteral("ANTHROPIC_API_KEY")
        || variable == QStringLiteral("OPENAI_API_KEY")
        || variable == QStringLiteral("GEMINI_API_KEY");
}

void read(const QString& variable, QObject* receiver, ReadCallback callback)
{
    if (!supportedProvider(variable) || !receiver) {
        callback(false, {}, QStringLiteral("Invalid provider"));
        return;
    }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(Service, receiver);
    job->setKey(entry(variable));
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, receiver,
            [job, callback](QKeychain::Job*) {
        const auto error = job->error();
        const bool ok = error == QKeychain::NoError
            || error == QKeychain::EntryNotFound;
        const QString text = error == QKeychain::NoError ? job->textData() : QString();
        const QString detail = ok ? QString() : job->errorString();
        callback(ok, text, detail);
        job->deleteLater();
    });
    job->start();
#else
    callback(false, {}, QStringLiteral("QtKeychain is not enabled in this build"));
#endif
}

void write(const QString& variable, const QString& secret,
           QObject* receiver, StatusCallback callback)
{
    if (!supportedProvider(variable) || secret.isEmpty() || !receiver) {
        callback(false, QStringLiteral("Invalid provider or empty key"));
        return;
    }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(Service, receiver);
    job->setKey(entry(variable));
    job->setTextData(secret);
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, receiver,
            [job, callback](QKeychain::Job*) {
        const bool ok = job->error() == QKeychain::NoError;
        const QString detail = ok ? QString() : job->errorString();
        callback(ok, detail);
        job->deleteLater();
    });
    job->start();
#else
    callback(false, QStringLiteral("QtKeychain is not enabled in this build"));
#endif
}

void remove(const QString& variable, QObject* receiver, StatusCallback callback)
{
    if (!supportedProvider(variable) || !receiver) {
        callback(false, QStringLiteral("Invalid provider"));
        return;
    }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
    auto* job = new QKeychain::DeletePasswordJob(Service, receiver);
    job->setKey(entry(variable));
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, receiver,
            [job, callback](QKeychain::Job*) {
        const bool ok = job->error() == QKeychain::NoError
            || job->error() == QKeychain::EntryNotFound;
        const QString detail = ok ? QString() : job->errorString();
        callback(ok, detail);
        job->deleteLater();
    });
    job->start();
#else
    callback(false, QStringLiteral("QtKeychain is not enabled in this build"));
#endif
}
} // namespace HermesCredentialStore
} // namespace Mattermost
