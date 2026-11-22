#include "WorkspaceLayout.h"
#include "backend/AccountIdentity.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <utility>

namespace Mattermost {
namespace {

bool reject(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}

bool uniqueExistingIds(const QStringList& ids, const QSet<QString>& allowed)
{
    QSet<QString> seen;
    for (const QString& id : ids) {
        if (id.isEmpty() || !allowed.contains(id) || seen.contains(id))
            return false;
        seen.insert(id);
    }
    return true;
}

QJsonArray jsonIds(const QStringList& ids)
{
    QJsonArray result;
    for (const QString& id : ids) result.append(id);
    return result;
}

bool parseIds(const QJsonValue& value, QStringList& result)
{
    if (!value.isArray()) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) return false;
        result.append(item.toString());
    }
    return true;
}

bool hasString(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).isString();
}

QString remoteIdentityKey(const WorkspaceLayout::Account& account)
{
    if (!account.hasConnectionIdentity())
        return {};
    const QUrl url(account.serverBaseUrl);
    // Configuration is a plain server base URL, not a navigable link.
    if (!url.isValid() || url.hasQuery() || url.hasFragment())
        return {};
    return accountIdentityKey(account.serverBaseUrl, account.serverUserId);
}

} // namespace

namespace {

QString newAccountId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString accountIdentity(const WorkspaceLayout::Account& record)
{
    return record.hasConnectionIdentity()
        ? accountIdentityKey(record.serverBaseUrl, record.serverUserId)
        : QString();
}

} // namespace

const WorkspaceLayout::Account* WorkspaceLayout::account(const QString& accountId) const
{
    for (const Account& record : accounts)
        if (record.id == accountId)
            return &record;
    return nullptr;
}

QString WorkspaceLayout::upsertAuthenticatedAccount(
    const QString& serverBaseUrl, const QString& serverUserId,
    const QString& loginName, const QString& displayName)
{
    if (loginName.trimmed().isEmpty() || displayName.trimmed().isEmpty())
        return {};
    const QUrl url(serverBaseUrl);
    if (!url.isValid() || url.hasQuery() || url.hasFragment())
        return {};
    const QString key = accountIdentityKey(serverBaseUrl, serverUserId);
    if (key.isEmpty())
        return {};

    WorkspaceLayout next = *this;
    for (Account& record : next.accounts) {
        if (accountIdentity(record) != key)
            continue;
        record.loginName = loginName;
        if (!next.validate())
            return {};
        const QString existingId = record.id;
        *this = std::move(next);
        return existingId;
    }

    Account record;
    record.id = newAccountId();
    record.name = displayName;
    record.serverBaseUrl = serverBaseUrl;
    record.serverUserId = serverUserId;
    record.loginName = loginName;
    const QString addedId = record.id;
    next.accounts.append(record);

    if (next.profiles.isEmpty()) {
        Profile profile;
        profile.id = newAccountId();
        profile.name = QStringLiteral("Simultaneous");
        profile.accountIds.append(addedId);
        next.profiles.append(profile);
    } else {
        QString selectedProfile;
        for (const View& view : next.views) {
            if (view.id == next.activeViewId) {
                selectedProfile = view.profileId;
                break;
            }
        }
        bool appended = false;
        for (Profile& profile : next.profiles) {
            if (selectedProfile.isEmpty() || profile.id == selectedProfile) {
                profile.accountIds.append(addedId);
                selectedProfile = profile.id;
                appended = true;
                break;
            }
        }
        if (!appended)
            return {};
    }

    View view;
    view.id = newAccountId();
    for (const Profile& profile : next.profiles) {
        if (profile.accountIds.contains(addedId)) {
            view.profileId = profile.id;
            break;
        }
    }
    view.name = displayName;
    view.accountIds.append(addedId);
    next.views.append(view);
    next.activeViewId = view.id;
    if (!next.validate())
        return {};
    *this = std::move(next);
    return addedId;
}

bool WorkspaceLayout::resolveUnconfiguredAccount(
    const QString& accountId, const QString& serverBaseUrl,
    const QString& serverUserId, const QString& loginName)
{
    if (loginName.trimmed().isEmpty())
        return false;
    WorkspaceLayout next = *this;
    for (Account& record : next.accounts) {
        if (record.id != accountId)
            continue;
        if (record.hasConnectionIdentity())
            return false;
        record.serverBaseUrl = serverBaseUrl;
        record.serverUserId = serverUserId;
        record.loginName = loginName;
        if (!next.validate())
            return false;
        *this = std::move(next);
        return true;
    }
    return false;
}

bool WorkspaceLayout::selectAccount(const QString& accountId)
{
    if (!account(accountId))
        return false;

    for (const View& view : views) {
        if (view.id == activeViewId && view.accountIds.contains(accountId))
            return true;
    }
    QString preferredProfile;
    for (const View& view : views)
        if (view.id == activeViewId)
            preferredProfile = view.profileId;

    for (const View& view : views) {
        if (view.profileId == preferredProfile && view.accountIds.contains(accountId))
            return selectView(view.id);
    }
    for (const View& view : views)
        if (view.accountIds.contains(accountId))
            return selectView(view.id);
    return false;
}

const WorkspaceLayout::Profile* WorkspaceLayout::findProfile(const QString& id) const
{
    for (const Profile& profile : profiles)
        if (profile.id == id) return &profile;
    return nullptr;
}

const WorkspaceLayout::View* WorkspaceLayout::findView(const QString& id) const
{
    for (const View& view : views)
        if (view.id == id) return &view;
    return nullptr;
}

bool WorkspaceLayout::validate(QString* error) const
{
    if (accounts.isEmpty() || profiles.isEmpty() || views.isEmpty())
        return reject(error, QStringLiteral("Missing accounts, profiles or views"));

    QSet<QString> accountIds;
    QSet<QString> identities;
    for (const Account& account : accounts) {
        if (account.id.isEmpty() || account.name.isEmpty()
            || accountIds.contains(account.id))
            return reject(error, QStringLiteral("Invalid or duplicate account"));
        const bool anyIdentity = !account.serverBaseUrl.isEmpty()
            || !account.serverUserId.isEmpty() || !account.loginName.isEmpty();
        if (anyIdentity) {
            if (!account.hasConnectionIdentity() || account.loginName.trimmed().isEmpty())
                return reject(error, QStringLiteral("Incomplete account identity"));
            const QString key = remoteIdentityKey(account);
            if (key.isEmpty() || identities.contains(key))
                return reject(error, QStringLiteral("Invalid or duplicate remote account"));
            identities.insert(key);
        }
        accountIds.insert(account.id);
    }

    QSet<QString> profileIds;
    for (const Profile& profile : profiles) {
        if (profile.id.isEmpty() || profile.name.isEmpty()
            || profileIds.contains(profile.id)
            || profile.accountIds.isEmpty()
            || !uniqueExistingIds(profile.accountIds, accountIds))
            return reject(error, QStringLiteral("Invalid profile membership"));
        profileIds.insert(profile.id);
    }

    QSet<QString> viewIds;
    QSet<QString> profilesWithViews;
    for (const View& view : views) {
        const Profile* profile = findProfile(view.profileId);
        if (view.id.isEmpty() || view.name.isEmpty() || viewIds.contains(view.id)
            || !profile || view.accountIds.isEmpty())
            return reject(error, QStringLiteral("Invalid view"));
        const QSet<QString> allowed(profile->accountIds.cbegin(),
                                    profile->accountIds.cend());
        if (!uniqueExistingIds(view.accountIds, allowed))
            return reject(error, QStringLiteral("View is not a subset of its profile"));
        if (view.directMessagesSorting != QLatin1String("recent")
            && view.directMessagesSorting != QLatin1String("alpha"))
            return reject(error, QStringLiteral("Unsupported view DM/GM sorting"));
        viewIds.insert(view.id);
        profilesWithViews.insert(view.profileId);
    }

    for (const Profile& profile : profiles) {
        if (!profilesWithViews.contains(profile.id))
            return reject(error, QStringLiteral("Profile has no view"));
    }

    if (!viewIds.contains(activeViewId))
        return reject(error, QStringLiteral("Active view does not exist"));

    if (error) error->clear();
    return true;
}

std::optional<WorkspaceLayout::Transition>
WorkspaceLayout::transitionTo(const QString& viewId) const
{
    if (!validate()) return std::nullopt;
    const View* next = findView(viewId);
    const View* current = findView(activeViewId);
    if (!next || !current) return std::nullopt;

    const Profile* nextProfile = findProfile(next->profileId);
    const Profile* oldProfile = findProfile(current->profileId);
    if (!nextProfile || !oldProfile) return std::nullopt;

    Transition transition;
    transition.targetProfileId = nextProfile->id;
    transition.targetViewId = next->id;
    transition.visibleAccountIds = next->accountIds;

    const QSet<QString> oldIds(oldProfile->accountIds.cbegin(),
                               oldProfile->accountIds.cend());
    const QSet<QString> newIds(nextProfile->accountIds.cbegin(),
                               nextProfile->accountIds.cend());
    for (const QString& id : oldProfile->accountIds)
        if (!newIds.contains(id)) transition.disconnectAccountIds.append(id);
    for (const QString& id : nextProfile->accountIds)
        if (!oldIds.contains(id)) transition.connectAccountIds.append(id);
    return transition;
}

bool WorkspaceLayout::selectView(const QString& viewId)
{
    if (!transitionTo(viewId)) return false;
    activeViewId = viewId;
    return true;
}

QByteArray WorkspaceLayout::toJson() const
{
    if (!validate()) return {};

    QJsonArray accountArray;
    for (const Account& account : accounts)
        accountArray.append(QJsonObject{
            {QStringLiteral("id"), account.id},
            {QStringLiteral("name"), account.name},
            {QStringLiteral("serverBaseUrl"), account.serverBaseUrl},
            {QStringLiteral("serverUserId"), account.serverUserId},
            {QStringLiteral("loginName"), account.loginName}});

    QJsonArray profileArray;
    for (const Profile& profile : profiles)
        profileArray.append(QJsonObject{
            {QStringLiteral("id"), profile.id},
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("accountIds"), jsonIds(profile.accountIds)}});

    QJsonArray viewArray;
    for (const View& view : views)
        viewArray.append(QJsonObject{
            {QStringLiteral("id"), view.id},
            {QStringLiteral("profileId"), view.profileId},
            {QStringLiteral("name"), view.name},
            {QStringLiteral("accountIds"), jsonIds(view.accountIds)},
            {QStringLiteral("directMessagesSorting"), view.directMessagesSorting}});

    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 2},
        {QStringLiteral("accounts"), accountArray},
        {QStringLiteral("profiles"), profileArray},
        {QStringLiteral("views"), viewArray},
        {QStringLiteral("activeViewId"), activeViewId}
    }).toJson(QJsonDocument::Compact);
}

std::optional<WorkspaceLayout>
WorkspaceLayout::fromJson(const QByteArray& json, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reject(error, QStringLiteral("Invalid workspace JSON"));
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != 1 && version != 2) {
        reject(error, QStringLiteral("Unsupported workspace version"));
        return std::nullopt;
    }
    const QJsonValue accountsValue = root.value(QStringLiteral("accounts"));
    const QJsonValue profilesValue = root.value(QStringLiteral("profiles"));
    const QJsonValue viewsValue = root.value(QStringLiteral("views"));
    if (!accountsValue.isArray() || !profilesValue.isArray()
        || !viewsValue.isArray() || !hasString(root, "activeViewId")) {
        reject(error, QStringLiteral("Malformed workspace fields"));
        return std::nullopt;
    }

    WorkspaceLayout layout;
    layout.activeViewId = root.value(QStringLiteral("activeViewId")).toString();

    for (const QJsonValue& item : accountsValue.toArray()) {
        if (!item.isObject() || !hasString(item.toObject(), "id")
            || !hasString(item.toObject(), "name")) {
            reject(error, QStringLiteral("Malformed account"));
            return std::nullopt;
        }
        const QJsonObject object = item.toObject();
        Account account;
        account.id = object.value(QStringLiteral("id")).toString();
        account.name = object.value(QStringLiteral("name")).toString();
        if (version == 2) {
            if (!hasString(object, "serverBaseUrl")
                || !hasString(object, "serverUserId")
                || !hasString(object, "loginName")) {
                reject(error, QStringLiteral("Malformed account identity"));
                return std::nullopt;
            }
            account.serverBaseUrl = object.value(QStringLiteral("serverBaseUrl")).toString();
            account.serverUserId = object.value(QStringLiteral("serverUserId")).toString();
            account.loginName = object.value(QStringLiteral("loginName")).toString();
        }
        layout.accounts.append(account);
    }

    for (const QJsonValue& item : profilesValue.toArray()) {
        if (!item.isObject() || !hasString(item.toObject(), "id")
            || !hasString(item.toObject(), "name")) {
            reject(error, QStringLiteral("Malformed profile"));
            return std::nullopt;
        }
        const QJsonObject object = item.toObject();
        Profile profile;
        profile.id = object.value(QStringLiteral("id")).toString();
        profile.name = object.value(QStringLiteral("name")).toString();
        if (!parseIds(object.value(QStringLiteral("accountIds")),
                      profile.accountIds)) {
            reject(error, QStringLiteral("Malformed profile account IDs"));
            return std::nullopt;
        }
        layout.profiles.append(profile);
    }

    for (const QJsonValue& item : viewsValue.toArray()) {
        if (!item.isObject() || !hasString(item.toObject(), "id")
            || !hasString(item.toObject(), "name")
            || !hasString(item.toObject(), "profileId")) {
            reject(error, QStringLiteral("Malformed view"));
            return std::nullopt;
        }
        const QJsonObject object = item.toObject();
        View view;
        view.id = object.value(QStringLiteral("id")).toString();
        view.profileId = object.value(QStringLiteral("profileId")).toString();
        view.name = object.value(QStringLiteral("name")).toString();
        if (!parseIds(object.value(QStringLiteral("accountIds")),
                      view.accountIds)) {
            reject(error, QStringLiteral("Malformed view account IDs"));
            return std::nullopt;
        }
        // v1/v2 catalogs written before merged sidebars had no local sorting
        // preference. Missing means recent; malformed/unknown values must
        // not silently override a user's persisted choice.
        if (object.contains(QStringLiteral("directMessagesSorting"))) {
            if (!hasString(object, "directMessagesSorting")) {
                reject(error, QStringLiteral("Malformed view DM/GM sorting"));
                return std::nullopt;
            }
            view.directMessagesSorting =
                object.value(QStringLiteral("directMessagesSorting")).toString();
        }
        layout.views.append(view);
    }

    if (!layout.validate(error)) return std::nullopt;
    return layout;
}

} // namespace Mattermost
