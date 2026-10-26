#pragma once

#include <functional>

#include <QObject>
#include <QString>

namespace Mattermost {

/** Asynchronous platform keyring storage; never falls back to plaintext. */
class SessionTokenStore
{
public:
    enum class ReadStatus { Found, NotFound, Unavailable };

    using ReadCallback = std::function<void(ReadStatus, const QString&)>;
    using WriteCallback = std::function<void(bool)>;

    static void read(QObject* context, const QString& server,
                     const QString& username, ReadCallback callback);
    static void write(QObject* context, const QString& server,
                      const QString& username, const QString& token,
                      WriteCallback callback = {});
    static void remove(QObject* context, const QString& server,
                       const QString& username);

private:
    static QString keyFor(const QString& server, const QString& username);
};

} // namespace Mattermost
