#include "AccountRegistry.h"

#include <QUrl>
#include <QUuid>

#include "backend/AccountIdentity.h"

namespace Mattermost {
namespace {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString identity(const WorkspaceLayout::Account& account)
{
    return account.hasConnectionIdentity()
        ? accountIdentityKey(account.serverBaseUrl, account.serverUserId) : QString();
}

} // namespace

bool AccountRegistry::load(const QByteArray& json, QString* error)
{
    if (json.isEmpty()) {
        _layout = WorkspaceLayout {};
        if (error)
            error->clear();
        return true;
    }
    auto parsed = WorkspaceLayout::fromJson(json, error);
    if (!parsed)
        return false; // Preserve the last valid layout on malformed input.
    _layout = std::move(*parsed);
    return true;
}

const WorkspaceLayout::Account* AccountRegistry::account(const QString& id) const
{
    for (const auto& record : _layout.accounts)
        if (record.id == id)
            return &record;
    return nullptr;
}

QString AccountRegistry::registerAuthenticated(const QString& serverBaseUrl,
                                               const QString& serverUserId,
                                               const QString& loginName,
                                               const QString& displayName)
{
    if (loginName.trimmed().isEmpty() || displayName.trimmed().isEmpty())
        return {};

    const QUrl url(serverBaseUrl);
    if (!url.isValid() || url.hasQuery() || url.hasFragment())
        return {};
    const QString key = accountIdentityKey(serverBaseUrl, serverUserId);
    if (key.isEmpty())
        return {};

    WorkspaceLayout next = _layout;
    for (auto& record : next.accounts) {
        if (identity(record) != key)
            continue;
        record.loginName = loginName;
        if (!next.validate())
            return {};
        _layout = std::move(next);
        return record.id;
    }

    WorkspaceLayout::Account record;
    record.id = newId();
    record.name = displayName;
    record.serverBaseUrl = serverBaseUrl;
    record.serverUserId = serverUserId;
    record.loginName = loginName;
    const QString newAccountId = record.id;
    next.accounts.append(record);

    if (next.profiles.isEmpty()) {
        WorkspaceLayout::Profile profile;
        profile.id = newId();
        profile.name = QStringLiteral("Simultaneous");
        profile.accountIds.append(newAccountId);
        next.profiles.append(profile);
    } else {
        QString selectedProfile;
        for (const auto& view : next.views) {
            if (view.id == next.activeViewId) {
                selectedProfile = view.profileId;
                break;
            }
        }
        bool added = false;
        for (auto& profile : next.profiles) {
            if (selectedProfile.isEmpty() || profile.id == selectedProfile) {
                profile.accountIds.append(newAccountId);
                selectedProfile = profile.id;
                added = true;
                break;
            }
        }
        if (!added)
            return {};
    }

    WorkspaceLayout::View view;
    view.id = newId();
    view.profileId = next.profiles.isEmpty() ? QString() : next.profiles.first().id;
    for (const auto& profile : next.profiles) {
        if (profile.accountIds.contains(newAccountId)) {
            view.profileId = profile.id;
            break;
        }
    }
    view.name = displayName;
    view.accountIds.append(newAccountId);
    next.views.append(view);
    next.activeViewId = view.id;
    if (!next.validate())
        return {};
    _layout = std::move(next);
    return newAccountId;
}

bool AccountRegistry::selectAccount(const QString& id)
{
    if (!account(id))
        return false;
    // Prefer the current view/profile when an account belongs to multiple
    // views. Never change connection membership simply to select a sidebar.
    for (const auto& view : _layout.views) {
        if (view.id == _layout.activeViewId && view.accountIds.contains(id))
            return true;
    }
    QString preferredProfile;
    for (const auto& view : _layout.views)
        if (view.id == _layout.activeViewId)
            preferredProfile = view.profileId;
    for (const auto& view : _layout.views) {
        if (view.profileId == preferredProfile && view.accountIds.contains(id))
            return _layout.selectView(view.id);
    }
    for (const auto& view : _layout.views)
        if (view.accountIds.contains(id))
            return _layout.selectView(view.id);
    return false;
}

} // namespace Mattermost
