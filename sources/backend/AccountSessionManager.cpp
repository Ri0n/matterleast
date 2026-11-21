#include "AccountSessionManager.h"

#include <QUrl>

#include "Backend.h"
#include "types/BackendUser.h"

namespace Mattermost {

AccountSessionManager::AccountSessionManager(QObject* parent)
    : QObject(parent)
{
}

AccountSessionManager::~AccountSessionManager() = default;

Backend& AccountSessionManager::createLoginBackend()
{
    // QObject parentage is the sole owner. Indexes below are deliberately
    // non-owning, so deleting a session cannot invoke two independent owners.
    auto* backend = new Backend(this);
    _sessions.append(backend);
    return *backend;
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

bool AccountSessionManager::registerAuthenticated(Backend& backend)
{
    if (!_sessions.contains(&backend) || !backend.loggedIn()
        || !backend.getStorage().loginUser) {
        return false;
    }

    const QString key = identityKey(backend.networkSession().host(),
                                    backend.getStorage().loginUser->id);
    if (key.isEmpty())
        return false;

    Backend* existing = _registered.value(key, nullptr);
    if (existing && existing != &backend)
        return false;

    auto it = _registrations.find(&backend);
    if (it != _registrations.end() && it->key == key)
        return true;

    if (it != _registrations.end())
        unregisterAccount(backend);

    Registration registration;
    registration.key = key;
    // Backend::reset() disconnects all its signals. This observer is
    // installed on every successful registration and removed on logout.
    registration.endingConnection = connect(
        &backend, &Backend::sessionEnding, this,
        [this, &backend] { unregisterAccount(backend); });
    _registrations.insert(&backend, registration);
    _registered.insert(key, &backend);

    emit accountRegistered(&backend);
    if (!_activeBackend)
        changeActiveBackend(&backend);
    return true;
}

bool AccountSessionManager::unregisterAccount(Backend& backend)
{
    auto it = _registrations.find(&backend);
    if (it == _registrations.end())
        return false;

    _registered.remove(it->key);
    QObject::disconnect(it->endingConnection);
    _registrations.erase(it);
    emit accountUnregistered(&backend);

    if (_activeBackend == &backend)
        changeActiveBackend(firstRegisteredBackend());
    return true;
}

Backend* AccountSessionManager::findAccount(const QString& serverBaseUrl,
                                            const QString& serverUserId) const
{
    const QString key = identityKey(serverBaseUrl, serverUserId);
    return key.isEmpty() ? nullptr : _registered.value(key, nullptr);
}

QVector<Backend*> AccountSessionManager::registeredBackends() const
{
    QVector<Backend*> result;
    result.reserve(_registered.size());
    for (Backend* backend : _sessions) {
        if (_registrations.contains(backend))
            result.append(backend);
    }
    return result;
}

Backend* AccountSessionManager::firstRegisteredBackend() const
{
    for (Backend* backend : _sessions) {
        if (_registrations.contains(backend))
            return backend;
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
    if (!_registrations.contains(&backend) || !backend.loggedIn())
        return false;
    changeActiveBackend(&backend);
    return true;
}

bool AccountSessionManager::removeBackend(Backend& backend)
{
    if (!_sessions.contains(&backend) || backend.loggedIn()
        || _registrations.contains(&backend)) {
        return false;
    }

    // Callers first finish the login dialog / destroy the account view.
    // QObject teardown disconnects signal slots and safely deletes all
    // Backend-owned services, HTTP connections and timers.
    _sessions.removeAll(&backend);
    if (_activeBackend == &backend)
        changeActiveBackend(firstRegisteredBackend());
    delete &backend;
    return true;
}

} // namespace Mattermost
