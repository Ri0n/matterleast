#include "MigrationManager.h"

#include <QPointer>
#include <QUrl>
#include <utility>

#include "backend/AccountManager.h"
#include "backend/Account.h"
#include "backend/Backend.h"
#include "backend/SessionTokenStore.h"
#include "backend/types/BackendLoginData.h"
#include "backend/types/BackendUser.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {

// Schema migrations are chronological and append-only. A step's implementation
// date lets maintainers assess when old format support can be retired.
// v1 — implemented 2026-11-22: import pre-multi-account domain/username/token.
// Keep this logic here; do not resurrect auto-login in LoginDialog.
constexpr MigrationManager::Step kSteps[] {
    {1, "2026-11-22", "legacy-single-account-login"}
};

const QString kVersionKey = QStringLiteral("migrations/version");
const QString kLegacyDomain = QStringLiteral("domain");
const QString kLegacyUsername = QStringLiteral("username");
const QString kLegacyToken = QStringLiteral("token");

} // namespace

const MigrationManager::Step* MigrationManager::steps(int* count)
{
    if (count)
        *count = int(sizeof(kSteps) / sizeof(kSteps[0]));
    return kSteps;
}

MigrationManager::MigrationManager(AccountManager& accounts, QObject* parent)
    : QObject(parent)
    , _accounts(accounts)
    , _options(MLOptions::instance())
{
    // A successful manual login can also complete an interrupted legacy
    // migration. Only a *confirmed* keyring write permits dropping plaintext.
    connect(&_accounts, &AccountManager::credentialsStored, this,
            [this](const QString& server, const QString& login) {
        if (_options->value<int>(kVersionKey) >= LatestVersion)
            return;
        const QUrl stored(_options->value<QString>(kLegacyDomain));
        const QUrl accepted(server);
        if (stored.adjusted(QUrl::StripTrailingSlash) !=
                accepted.adjusted(QUrl::StripTrailingSlash)
            || _options->value<QString>(kLegacyUsername) != login)
            return;
        completeLegacyMigration();
    });
}

QString MigrationManager::legacyServerHint() const
{
    return _options->value<QString>(kLegacyDomain);
}

QString MigrationManager::legacyLoginHint() const
{
    return _options->value<QString>(kLegacyUsername);
}

void MigrationManager::finish(Result result)
{
    _running = false;
    auto callback = std::move(_completion);
    _completion = {};
    if (callback)
        callback(result);
}

void MigrationManager::completeLegacyMigration()
{
    // The imported layout and keychain entry have already been persisted.
    // Wipe the old plaintext secret *before* recording the completed version:
    // a crash in between is safe to replay.
    _options->setValue<QString>(kLegacyToken, {});
    _options->setValue<QString>(kLegacyDomain, {});
    _options->setValue<QString>(kLegacyUsername, {});
    _options->setValue<int>(kVersionKey, 1);
}

void MigrationManager::run(Completion callback)
{
    if (_running) {
        if (callback)
            callback(Result::Unavailable);
        return;
    }
    _completion = std::move(callback);
    const int current = _options->value<int>(kVersionKey, 0);
    if (current < 0 || current > LatestVersion) {
        finish(Result::UnsupportedVersion);
        return;
    }
    if (current == LatestVersion) {
        finish(Result::Complete);
        return;
    }
    _running = true;
    migrateLegacyLogin();
}

void MigrationManager::migrateLegacyLogin()
{
    // Never convert global defaults into an extra server alongside an already
    // configured multi-account catalog. Preserve unmigrated secrets for a
    // deliberate recovery instead of silently losing them.
    if (!_accounts.empty()) {
        if (_options->value<QString>(kLegacyToken).isEmpty())
            completeLegacyMigration();
        finish(Result::Complete);
        return;
    }
    const QString server = legacyServerHint();
    const QString login = legacyLoginHint();
    const QString plaintext = _options->value<QString>(kLegacyToken);
    if (server.isEmpty() && login.isEmpty() && plaintext.isEmpty()) {
        completeLegacyMigration();
        finish(Result::Complete);
        return;
    }
    if (server.isEmpty() || login.isEmpty()) {
        finish(Result::ManualSignInRequired);
        return;
    }

    QPointer<MigrationManager> guard(this);
    SessionTokenStore::read(this, server, login,
        [this, guard, server, login, plaintext](
            SessionTokenStore::ReadStatus status, const QString& secureToken) {
        if (!guard || !_running)
            return;
        if (status == SessionTokenStore::ReadStatus::Found) {
            authenticateLegacy(server, login, secureToken, true);
        } else if (status == SessionTokenStore::ReadStatus::NotFound
                   && !plaintext.isEmpty()) {
            authenticateLegacy(server, login, plaintext, false);
        } else {
            // Keyring unavailable is NOT the same as no entry. Do not erase
            // a still-usable legacy token, nor mark a failed migration.
            finish(status == SessionTokenStore::ReadStatus::Unavailable
                ? Result::Unavailable : Result::ManualSignInRequired);
        }
    });
}

void MigrationManager::authenticateLegacy(
    const QString& server, const QString& login,
    const QString& token, bool alreadyInKeychain)
{
    Backend* candidate = _accounts.createLoginBackend();
    QPointer<Backend> guardedCandidate(candidate);
    QPointer<MigrationManager> guard(this);

    BackendLoginData credentials;
    credentials.domain = server;
    credentials.username = login;
    credentials.token = token;
    candidate->login(credentials,
        [this, guard, guardedCandidate, server, login, token, alreadyInKeychain](
            const QString& verifiedToken) {
        if (!guard || !_running || !guardedCandidate)
            return;
        Backend* backend = guardedCandidate.data();
        if (verifiedToken.isEmpty()) {
            _accounts.discardCandidate(backend);
            finish(backend->lastLoginFailureTransient()
                   ? Result::Unavailable : Result::ManualSignInRequired);
            return;
        }
        const QString display = QUrl(backend->serverDomain()).host()
            + QStringLiteral(" — ") + backend->getLoginUser().username;

        const auto accept = [this, guard, guardedCandidate, login, display] {
            if (!guard || !_running || !guardedCandidate)
                return;
            if (!_accounts.adoptAuthenticated(
                    guardedCandidate.data(), login, display, {}, false)) {
                _accounts.discardCandidate(guardedCandidate.data());
                finish(Result::ManualSignInRequired);
                return;
            }
            completeLegacyMigration();
            finish(Result::Complete);
        };

        if (alreadyInKeychain) {
            accept();
        } else {
            // A plaintext token is written only after /users/me verified it.
            // Until keychain confirms success, the old settings remain intact
            // and no durable account record is committed.
            SessionTokenStore::write(this, server, login, verifiedToken,
                [this, guard, guardedCandidate, accept](bool written) {
                if (!guard || !_running)
                    return;
                if (!written) {
                    if (guardedCandidate)
                        _accounts.discardCandidate(guardedCandidate.data());
                    finish(Result::Unavailable);
                    return;
                }
                accept();
            });
        }
    });
}

} // namespace Mattermost
