#include "AccountSessionManager.h"

#include <QChildEvent>
#include <QUrl>

#include "Backend.h"
#include "types/BackendUser.h"

namespace Mattermost {

AccountSessionManager::AccountSessionManager(QObject* parent)
    : QObject(parent)
{
}

AccountSessionManager::~AccountSessionManager() = default;

Backend* AccountSessionManager::createLoginBackend()
{
    auto* backend = new Backend(this);
    _sessions.append(backend);
    // The direct observer covers ordinary QObject destruction. Backend::reset()
    // disconnects all Backend signals, so childEvent() also handles the case
    // where this observer has been disconnected before destruction.
    connect(backend, &QObject::destroyed, this,
            [this, backend] { forgetBackend(backend); });
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

bool AccountSessionManager::registerAuthenticated(Backend* backend)
{
    if (!_sessions.contains(backend) || !backend->loggedIn()
        || !backend->getStorage().loginUser) {
        return false;
    }

    const QString key = identityKey(backend->networkSession().host(),
                                    backend->getStorage().loginUser->id);
    if (key.isEmpty())
        return false;

    Backend* existing = _registered.value(key, nullptr);
    if (existing && existing != backend)
        return false;

    auto it = _registrations.find(backend);
    if (it != _registrations.end() && it->key == key)
        return true;

    if (it != _registrations.end())
        unregisterAccount(backend);

    Registration registration;
    registration.key = key;
    // Backend::reset() disconnects all its signals. Reinstall this session
    // observer after successful reauthentication.
    registration.endingConnection = connect(
        backend, &Backend::sessionEnding, this,
        [this, backend] { unregisterAccount(backend); });
    _registrations.insert(backend, registration);
    _registered.insert(key, backend);

    emit accountRegistered(backend);
    if (!_activeBackend)
        changeActiveBackend(backend);
    return true;
}

bool AccountSessionManager::unregisterAccount(Backend* backend)
{
    auto it = _registrations.find(backend);
    if (it == _registrations.end())
        return false;

    _registered.remove(it->key);
    QObject::disconnect(it->endingConnection);
    _registrations.erase(it);
    emit accountUnregistered(backend);

    if (_activeBackend == backend)
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

bool AccountSessionManager::activate(Backend* backend)
{
    if (!_registrations.contains(backend) || !backend->loggedIn())
        return false;
    changeActiveBackend(backend);
    return true;
}

void AccountSessionManager::forgetBackend(Backend* backend)
{
    if (!_sessions.removeAll(backend))
        return;

    auto it = _registrations.find(backend);
    if (it != _registrations.end()) {
        _registered.remove(it->key);
        QObject::disconnect(it->endingConnection);
        _registrations.erase(it);
        // The Backend may already be inside QObject destruction. Do not
        // publish its now-invalid pointer through accountUnregistered().
        // Normal logout sends accountUnregistered() before destruction.
    }
    if (_activeBackend == backend)
        changeActiveBackend(firstRegisteredBackend());
}

void AccountSessionManager::childEvent(QChildEvent* event)
{
    if (event->removed()) {
        // Backend::reset() can disconnect the destroyed observer. ChildRemoved
        // is delivered synchronously by QObject on deletion or reparenting.
        // Only compare pointer identity: the derived Backend may already have
        // finished its destructor, so qobject_cast/dereference is unsafe.
        for (Backend* backend : _sessions) {
            if (static_cast<QObject*>(backend) == event->child()) {
                forgetBackend(backend);
                break;
            }
        }
    }
    QObject::childEvent(event);
}

bool AccountSessionManager::removeBackend(Backend* backend)
{
    if (!_sessions.contains(backend) || backend->loggedIn()
        || _registrations.contains(backend)) {
        return false;
    }

    // The UI must already have released all references to this Backend.
    // Detach before deletion; the destroyed/ChildRemoved observers also
    // cover destruction initiated outside the manager.
    forgetBackend(backend);
    delete backend;
    return true;
}

} // namespace Mattermost
