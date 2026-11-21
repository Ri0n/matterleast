#include "AccountSessionManager.h"

#include <algorithm>

#include <QUrl>

#include "Backend.h"
#include "types/BackendUser.h"

namespace Mattermost {

struct AccountSessionManager::Session
{
    explicit Session(AccountSessionManager* owner)
        : backend(std::make_unique<Backend>(owner))
    {
    }

    std::unique_ptr<Backend> backend;
    QString key;
    QMetaObject::Connection endingConnection;
};

AccountSessionManager::AccountSessionManager(QObject* parent)
    : QObject(parent)
{
}

AccountSessionManager::~AccountSessionManager() = default;

Backend& AccountSessionManager::createLoginBackend()
{
    auto session = std::make_unique<Session>(this);
    Backend& backend = *session->backend;
    _sessions.push_back(std::move(session));
    return backend;
}

QString AccountSessionManager::identityKey(const QString& serverBaseUrl,
                                            const QString& serverUserId)
{
    if (serverUserId.isEmpty() || serverUserId.contains(QChar(0x1f)))
        return {};

    QUrl url(serverBaseUrl);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("https")
            && scheme != QLatin1String("http"))
        || !url.userInfo().isEmpty()) {
        return {};
    }

    // Preserve the Mattermost installation path: two installations on the
    // same hostname may be different accounts. Canonicalize only URL spelling.
    url.setQuery(QString());
    url.setFragment(QString());
    if ((scheme == QLatin1String("https") && url.port() == 443)
        || (scheme == QLatin1String("http") && url.port() == 80)) {
        url.setPort(-1);
    }
    QString path = url.path();
    if (path.isEmpty() || !path.endsWith(QLatin1Char('/')))
        path.append(QLatin1Char('/'));
    url.setPath(path);

    return url.toString(QUrl::FullyEncoded) + QChar(0x1f) + serverUserId;
}

AccountSessionManager::Session* AccountSessionManager::sessionFor(
    const Backend* backend) const
{
    for (const auto& entry : _sessions) {
        if (entry->backend.get() == backend)
            return entry.get();
    }
    return nullptr;
}

bool AccountSessionManager::registerAuthenticated(Backend& backend)
{
    Session* entry = sessionFor(&backend);
    if (!entry || !backend.loggedIn() || !backend.getStorage().loginUser)
        return false;

    const QString key = identityKey(backend.networkSession().host(),
                                    backend.getStorage().loginUser->id);
    if (key.isEmpty())
        return false;

    Session* existing = _registered.value(key, nullptr);
    if (existing && existing != entry)
        return false;

    if (entry->key == key)
        return true;

    if (!entry->key.isEmpty())
        unregisterAccount(backend);

    entry->key = key;
    _registered.insert(key, entry);

    // Backend::reset() disconnects all signals. Reinstall the lifecycle
    // observer on each successful registration, including reauthentication.
    QObject::disconnect(entry->endingConnection);
    entry->endingConnection = connect(
        &backend, &Backend::sessionEnding, this,
        [this, &backend] { unregisterAccount(backend); });

    emit accountRegistered(&backend);
    if (!_activeBackend)
        changeActiveBackend(&backend);
    return true;
}

bool AccountSessionManager::unregisterAccount(Backend& backend)
{
    Session* entry = sessionFor(&backend);
    if (!entry || entry->key.isEmpty())
        return false;

    _registered.remove(entry->key);
    entry->key.clear();
    QObject::disconnect(entry->endingConnection);
    entry->endingConnection = {};
    emit accountUnregistered(&backend);

    if (_activeBackend == &backend)
        changeActiveBackend(firstRegisteredBackend());
    return true;
}

Backend* AccountSessionManager::findAccount(const QString& serverBaseUrl,
                                            const QString& serverUserId) const
{
    const QString key = identityKey(serverBaseUrl, serverUserId);
    Session* session = key.isEmpty() ? nullptr : _registered.value(key, nullptr);
    return session ? session->backend.get() : nullptr;
}

Backend* AccountSessionManager::firstRegisteredBackend() const
{
    for (const auto& session : _sessions) {
        if (!session->key.isEmpty())
            return session->backend.get();
    }
    return nullptr;
}

void AccountSessionManager::changeActiveBackend(Backend* backend)
{
    if (_activeBackend == backend)
        return;
    _activeBackend = backend;
    emit activeBackendChanged(backend);
}

bool AccountSessionManager::activate(Backend& backend)
{
    Session* entry = sessionFor(&backend);
    if (!entry || entry->key.isEmpty() || !backend.loggedIn())
        return false;
    changeActiveBackend(&backend);
    return true;
}

bool AccountSessionManager::removeBackend(Backend& backend)
{
    Session* entry = sessionFor(&backend);
    if (!entry || backend.loggedIn() || !entry->key.isEmpty())
        return false;

    const auto it = std::find_if(_sessions.begin(), _sessions.end(),
        [entry](const std::unique_ptr<Session>& session) {
            return session.get() == entry;
        });
    if (it == _sessions.end())
        return false;

    if (_activeBackend == &backend)
        changeActiveBackend(firstRegisteredBackend());
    _sessions.erase(it);
    return true;
}

} // namespace Mattermost
