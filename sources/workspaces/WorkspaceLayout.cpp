#include "WorkspaceLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

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

} // namespace

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
    for (const Account& account : accounts) {
        if (account.id.isEmpty() || account.name.isEmpty()
            || accountIds.contains(account.id))
            return reject(error, QStringLiteral("Invalid or duplicate account"));
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
            {QStringLiteral("name"), account.name}});

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
            {QStringLiteral("accountIds"), jsonIds(view.accountIds)}});

    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
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
    if (root.value(QStringLiteral("version")).toInt(-1) != 1) {
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
        layout.accounts.append({object.value(QStringLiteral("id")).toString(),
                                object.value(QStringLiteral("name")).toString()});
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
        layout.views.append(view);
    }

    if (!layout.validate(error)) return std::nullopt;
    return layout;
}

} // namespace Mattermost
