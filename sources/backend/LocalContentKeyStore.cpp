#include "LocalContentKeyStore.h"

#include "LocalMessageCrypto.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QVector>

#ifdef MATTERLEAST_HAVE_KEYCHAIN
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
constexpr auto ContentKeyName = "local-message-content-v1";

struct PendingCallback {
    QPointer<QObject> context;
    LocalContentKeyStore::Callback callback;
};

class KeyState final : public QObject
{
public:
    explicit KeyState(QObject* parent) : QObject(parent) {}

    void request(QObject* context, LocalContentKeyStore::Callback callback)
    {
        if (_completed) {
            QTimer::singleShot(0, context, [callback = std::move(callback),
                                             key = _encryptionKey] {
                callback(key);
            });
            return;
        }
        _callbacks.push_back({context, std::move(callback)});
        if (_started) return;
        _started = true;
        if (!LocalMessageCrypto::isAvailable()) {
            finish({});
            return;
        }
#ifdef MATTERLEAST_HAVE_KEYCHAIN
        auto* job = new QKeychain::ReadPasswordJob(QString::fromLatin1(KeychainService), this);
        job->setKey(QString::fromLatin1(ContentKeyName));
        job->setInsecureFallback(false);
        connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* finished) {
            if (finished->error() == QKeychain::NoError) {
                auto* read = static_cast<QKeychain::ReadPasswordJob*>(finished);
                const QByteArray key = QByteArray::fromBase64(read->textData().toLatin1());
                finish(key.size() == LocalMessageCrypto::KeyBytes ? key : QByteArray());
            } else if (finished->error() == QKeychain::EntryNotFound) {
                const QByteArray key = LocalMessageCrypto::randomKey();
                if (key.size() != LocalMessageCrypto::KeyBytes) {
                    finish({});
                    return;
                }
                auto* write = new QKeychain::WritePasswordJob(QString::fromLatin1(KeychainService), this);
                write->setKey(QString::fromLatin1(ContentKeyName));
                write->setTextData(QString::fromLatin1(key.toBase64()));
                write->setInsecureFallback(false);
                connect(write, &QKeychain::Job::finished, this,
                        [this, key](QKeychain::Job* done) {
                    finish(done->error() == QKeychain::NoError ? key : QByteArray());
                });
                write->start();
            } else {
                finish({});
            }
        });
        job->start();
#else
        finish({});
#endif
    }

private:
    void finish(const QByteArray& key)
    {
        _completed = true;
        _encryptionKey = key;
        auto pending = std::move(_callbacks);
        _callbacks.clear();
        for (auto& entry : pending) {
            if (entry.context && entry.callback) {
                QTimer::singleShot(0, entry.context,
                                   [callback = std::move(entry.callback), key] {
                    callback(key);
                });
            }
        }
    }

    bool _started = false;
    bool _completed = false;
    QByteArray _encryptionKey;
    QVector<PendingCallback> _callbacks;
};

KeyState* state()
{
    static KeyState* owner = new KeyState(QCoreApplication::instance());
    return owner;
}

} // namespace

void LocalContentKeyStore::request(QObject* context, Callback callback)
{
    if (!context || !callback) return;
    state()->request(context, std::move(callback));
}

} // namespace Mattermost
