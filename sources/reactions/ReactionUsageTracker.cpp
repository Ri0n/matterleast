#include "ReactionUsageTracker.h"

#include "options/MLOptions.h"

namespace Mattermost {
namespace {

const char* const reactionUsageSettingsKey = "reaction_usage/popularity_v1";

} // namespace

ReactionUsageTracker& ReactionUsageTracker::instance()
{
    static ReactionUsageTracker tracker;
    return tracker;
}

ReactionUsageTracker::ReactionUsageTracker()
{
    model_.restore(deserializeReactionUsage(
        MLOptions::instance()->value<QByteArray>(
            QString::fromLatin1(reactionUsageSettingsKey))));
}

void ReactionUsageTracker::recordUse(const QString& emojiName)
{
    if (emojiName.trimmed().isEmpty()) {
        return;
    }
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
    MLOptions::instance()->setValue(
        QString::fromLatin1(reactionUsageSettingsKey),
        serializeReactionUsage(model_.ranking()));
}

} // namespace Mattermost
