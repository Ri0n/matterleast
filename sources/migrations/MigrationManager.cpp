#include "MigrationManager.h"

#include <QLoggingCategory>
#include <QCryptographicHash>
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
#include "NavigationSnapshotMigration.h"
#include "navigation/ViewNavigationSession.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(migrationLog, "mattermost.migration")

const char* migrationResultName(MigrationManager::Result result)
{
    switch (result) {
    case MigrationManager::Result::Complete: return "complete";
    case MigrationManager::Result::ManualSignInRequired: return "manual-sign-in-required";
    case MigrationManager::Result::Unavailable: return "unavailable";
    case MigrationManager::Result::UnsupportedVersion: return "unsupported-version";
    }
    return "unknown";
}

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

void MigrationManager::cancel()
{
    if (!_running)
        return;
    qCInfo(migrationLog) << "Migration cancelled by an explicit login; step" << _currentStep;
    _running = false;
    _completion = {};
    QPointer<Backend> candidate = _candidate;
    _candidate.clear();
    if (candidate && candidate->parent() == &_accounts)
        _accounts.discardCandidate(candidate.data());
}

void MigrationManager::finish(Result result)
{
    // Dispatch future steps one by one only after the previous version was
    // durably recorded. A failed or deferred step must not advance the chain.
    if (result == Result::Complete && _running
        && _options->value<int>(kVersionKey, 0) >= _currentStep
        && _currentStep < LatestVersion) {
        executeStep(_currentStep + 1);
        return;
    }
    qCInfo(migrationLog) << "Migration finished" << migrationResultName(result)
                         << "step" << _currentStep
                         << "stored-version" << _options->value<int>(kVersionKey, 0);
    _running = false;
    _candidate.clear();
    auto callback = std::move(_completion);
    _completion = {};
    if (callback)
        callback(result);
}

void MigrationManager::executeStep(int version)
{
    _currentStep = version;
    qCInfo(migrationLog) << "Running migration step" << version;
    switch (version) {
    case 1:
        migrateLegacyLogin();
        return;
    default:
        finish(Result::UnsupportedVersion);
    }
}

bool MigrationManager::completeLegacyMigration()
{
    // The imported layout and keychain entry must already be durable.
    // A failed options sync must never cause the only available login
    // coordinates to be erased, even when the in-memory Account is alive.
    if (!_options->persistenceHealthy()) {
        qCWarning(migrationLog) << "Cannot complete migration: settings storage unavailable";
        return false;
    }

    // 2026-11-25: import legacy pinned tabs as part of the unreleased v1 step, when the
    // destination View has no durable snapshot. Never guess an account from
    // a remote channel ID, and never erase the legacy key before the new
    // View data has been durably written.
    auto* options = _options;
    QStringList migratedKeys;
    for (const auto& account : _accounts.layout().accounts) {
        if (account.serverBaseUrl.isEmpty() || account.serverUserId.isEmpty())
            continue;
        const QByteArray identity = (account.serverBaseUrl + QChar(0x1f)
                                    + account.serverUserId).toUtf8();
        const QString legacyKey = QStringLiteral("navigation_session/")
            + QString::fromLatin1(QCryptographicHash::hash(
                identity, QCryptographicHash::Sha256).toHex());
        const QByteArray legacy = options->value<QByteArray>(legacyKey);
        if (legacy.isEmpty()) continue;
        const WorkspaceLayout::View* target = nullptr;
        for (const auto& view : _accounts.layout().views)
            if (view.id == _accounts.layout().activeViewId
                && view.accountIds.contains(account.id)) {
                target = &view;
                break;
            }
        if (!target)
            for (const auto& view : _accounts.layout().views)
                if (view.accountIds.contains(account.id)) {
                    target = &view;
                    break;
                }
        if (!target) continue;
        // An existing View snapshot is authoritative, even when empty.
        const QString targetKey = QStringLiteral("navigation/views/") + target->id;
        if (!options->value<QByteArray>(targetKey).isEmpty()) continue;
        auto* session = _accounts.viewSession(target->id);
        if (!session || !NavigationSnapshotMigration::importLegacy(
                legacy, account.id, *session)) {
            qCWarning(migrationLog) << "Unable to import legacy navigation" << legacyKey;
            continue;
        }
        migratedKeys.append(legacyKey);
    }
    if (!migratedKeys.isEmpty()) {
        _accounts.persistViewSessions();
        if (options->persistenceHealthy()) {
            for (const QString& key : migratedKeys)
                options->setValue<QByteArray>(key, {});
        }
    }

    if (!_options->persistenceHealthy()) return false;

    // Wipe plaintext *before* recording the version. A crash during the
    // intermediate writes is safe to replay from the persisted Account.
    _options->setValue<QString>(kLegacyToken, {});
    if (!_options->persistenceHealthy())
        return false;
    _options->setValue<QString>(kLegacyDomain, {});
    _options->setValue<QString>(kLegacyUsername, {});
    if (!_options->persistenceHealthy())
        return false;
    _options->setValue<int>(kVersionKey, 1);
    const bool saved = _options->persistenceHealthy();
    if (!saved)
        qCWarning(migrationLog) << "Migration version could not be persisted";
    return saved;
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
    qCInfo(migrationLog) << "Migration startup: stored-version" << current
                         << "latest-version" << LatestVersion
                         << "catalog-accounts" << _accounts.layout().accounts.size();
    if (current < 0 || current > LatestVersion) {
        finish(Result::UnsupportedVersion);
        return;
    }
    if (current == LatestVersion) {
        qCInfo(migrationLog) << "No schema migration required";
        finish(Result::Complete);
        return;
    }
    _running = true;
    executeStep(current + 1);
}

void MigrationManager::migrateLegacyLogin()
{
    // Never convert global defaults into an extra server alongside an already
    // configured multi-account catalog. Preserve unmigrated secrets for a
    // deliberate recovery instead of silently losing them.
    if (!_accounts.empty()) {
        const bool noLegacySecret = _options->value<QString>(kLegacyToken).isEmpty();
        qCInfo(migrationLog) << "Catalog already exists, skipping legacy account import;"
                             << "legacy-plaintext-present" << !noLegacySecret;
        // Existing catalog alone is not evidence that its saved keyring entry
        // can be read. AccountManager logs that check during restoration.
        finish(noLegacySecret
            ? (completeLegacyMigration() ? Result::Complete : Result::Unavailable)
            : Result::ManualSignInRequired);
        return;
    }
    const QString server = legacyServerHint();
    const QString login = legacyLoginHint();
    const QString plaintext = _options->value<QString>(kLegacyToken);
    qCInfo(migrationLog) << "Legacy settings discovery:"
                         << "server-present" << !server.isEmpty()
                         << "login-present" << !login.isEmpty()
                         << "plaintext-present" << !plaintext.isEmpty();
    if (server.isEmpty() && login.isEmpty() && plaintext.isEmpty()) {
        finish(completeLegacyMigration()
            ? Result::Complete : Result::Unavailable);
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
        qCInfo(migrationLog) << "Legacy keyring read"
                             << (status == SessionTokenStore::ReadStatus::Found ? "found"
                                 : status == SessionTokenStore::ReadStatus::NotFound ? "not-found"
                                 : "unavailable");
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
    qCInfo(migrationLog) << "Verifying legacy credentials;"
                         << "source" << (alreadyInKeychain ? "keyring" : "legacy-settings");
    Backend* candidate = _accounts.createLoginBackend();
    _candidate = candidate;
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
            const bool transient = backend->lastLoginFailureTransient();
            qCWarning(migrationLog) << "Legacy credential verification failed;"
                                    << "transient" << transient;
            _accounts.discardCandidate(backend);
            finish(transient ? Result::Unavailable
                             : Result::ManualSignInRequired);
            return;
        }
        const QString display = QUrl(backend->serverDomain()).host()
            + QStringLiteral(" — ") + backend->getLoginUser().username;

        const auto accept = [this, guard, guardedCandidate, login, display] {
            if (!guard || !_running || !guardedCandidate)
                return;
            if (!_accounts.adoptAuthenticated(
                    guardedCandidate.data(), login, display, {}, false)) {
                qCWarning(migrationLog) << "Verified legacy session could not be registered";
                _accounts.discardCandidate(guardedCandidate.data());
                finish(Result::ManualSignInRequired);
                return;
            }
            qCInfo(migrationLog) << "Legacy account authenticated and adopted";
            finish(completeLegacyMigration()
                ? Result::Complete : Result::Unavailable);
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
                    qCWarning(migrationLog) << "Could not store verified legacy token in keyring";
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
