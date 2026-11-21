#pragma once

#include <functional>

#include <QObject>
#include <QString>

namespace Mattermost {

class AccountManager;
class Backend;
class MLOptions;

// Versioned, one-shot migrations. ALL pre-account legacy login/settings
// knowledge belongs under sources/migrations, never in LoginDialog or Backend.
class MigrationManager final : public QObject
{
public:
    struct Step {
        int version;
        const char* introducedOn; // ISO 8601; retain for removal decisions
        const char* name;
    };
    enum class Result { Complete, ManualSignInRequired, Unavailable, UnsupportedVersion };
    using Completion = std::function<void(Result)>;

    static constexpr int LatestVersion = 1;
    static const Step* steps(int* count);

    MigrationManager(AccountManager& accounts, QObject* parent = nullptr);

    void run(Completion callback);
    QString legacyServerHint() const;
    QString legacyLoginHint() const;

private:
    void migrateLegacyLogin();
    void authenticateLegacy(const QString& server, const QString& login,
                            const QString& token, bool alreadyInKeychain);
    void completeLegacyMigration();
    void finish(Result result);

    AccountManager& _accounts;
    MLOptions* const _options;
    Completion _completion;
    bool _running = false;
};

} // namespace Mattermost
