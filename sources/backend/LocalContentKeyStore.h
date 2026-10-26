#pragma once

#include <functional>

#include <QByteArray>
#include <QObject>

namespace Mattermost {

// Loads or creates one stable, random encryption key in the system keyring.
// Requests are asynchronous; an empty key means fail closed (no disk writes).
class LocalContentKeyStore
{
public:
    using Callback = std::function<void(const QByteArray&)>;

    static void request(QObject* context, Callback callback);
};

} // namespace Mattermost
