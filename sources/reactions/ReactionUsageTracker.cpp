#include "ReactionUsageTracker.h"

#include <map>
#include <QCryptographicHash>
#include <QUrl>

#include "backend/emoji/EmojiInfo.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {

const char* const legacyKey = "reaction_usage/popularity_v1";

QString serverKey(const QString& server)
{
    if (server.trimmed().isEmpty()) return {};
    QUrl url(server);
    url.setUserInfo(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    const auto canonical = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    return QStringLiteral("reaction_usage/servers/%1/popularity_v1").arg(
        QString::fromLatin1(QCryptographicHash::hash(canonical.toEncoded(),
            QCryptographicHash::Sha256).toHex()));
}

} // namespace

ReactionUsageTracker& ReactionUsageTracker::instance(const QString& server)
{
    // One bounded model per server URL, shared by accounts on that server.
    // A standalone Backend without an endpoint uses a non-persistent model.
    static std::map<QString, ReactionUsageTracker> trackers;
    const QString key = serverKey(server);
    auto it = trackers.find(key);
    if (it == trackers.end())
        it = trackers.emplace(key, ReactionUsageTracker(key)).first;
    return it->second;
}

ReactionUsageTracker::ReactionUsageTracker(const QString& key)
    : settingsKey_(key)
{
    if (!settingsKey_.isEmpty()) {
        auto* options = MLOptions::instance();
        if (options->contains(settingsKey_)) {
            model_.restore(deserializeReactionUsage(
                options->value<QByteArray>(settingsKey_)));
        } else {
            // The old global ranking cannot identify which server owns a
            // custom name. Carry over only globally defined built-in emoji.
            auto entries = deserializeReactionUsage(
                options->value<QByteArray>(QString::fromLatin1(legacyKey)));
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [](const ReactionUsageEntry& entry) {
                    return !EmojiInfo::resolveBuiltInByName(entry.name);
                }), entries.end());
            model_.restore(entries);
        }
    }
    if (model_.size() == 0)
        model_.seedIfEmpty(defaultReactionSeedNames());
    save();
}

void ReactionUsageTracker::recordUse(const QString& emojiName)
{
    if (emojiName.trimmed().isEmpty()) return;
    model_.recordUse(emojiName);
    save();
}

QVector<ReactionUsageEntry> ReactionUsageTracker::ranking() const
{
    return model_.ranking();
}

QStringList ReactionUsageTracker::topNames(int limit) const
{
    return model_.topNames(limit);
}

void ReactionUsageTracker::save() const
{
    if (!settingsKey_.isEmpty())
        MLOptions::instance()->setValue(settingsKey_,
            serializeReactionUsage(model_.ranking()));
}

} // namespace Mattermost
