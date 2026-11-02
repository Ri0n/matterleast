#pragma once

#include <functional>
#include <QString>

class QObject;

namespace Mattermost {
namespace HermesCredentialStore {
using ReadCallback = std::function<void(bool, const QString&, const QString&)>;
using StatusCallback = std::function<void(bool, const QString&)>;

bool available();
bool supportedProvider(const QString& variable);
void read(const QString& variable, QObject* receiver, ReadCallback callback);
void write(const QString& variable, const QString& secret,
           QObject* receiver, StatusCallback callback);
void remove(const QString& variable, QObject* receiver, StatusCallback callback);
} // namespace HermesCredentialStore
} // namespace Mattermost
