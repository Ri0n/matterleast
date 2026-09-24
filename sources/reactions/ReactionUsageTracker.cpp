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

    if (model_.size() == 0) {
        // Start with a small generic prior so a new profile has useful quick
        // reactions immediately. Real user choices reheat to 1.0 and quickly
        // dominate/evict these low-heat seed entries.
        model_.seedIfEmpty(defaultReactionSeedNames());
        save();
    }
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
