#pragma once

#include <QStringList>
#include <QVector>

#include "ReactionUsage.h"

namespace Mattermost {

class ReactionUsageTracker
{
public:
    static ReactionUsageTracker& instance(const QString& server);

    void recordUse(const QString& emojiName);
    QVector<ReactionUsageEntry> ranking() const;
    QStringList topNames(int limit = -1) const;

private:
    explicit ReactionUsageTracker(const QString& server);
    void save() const;

    QString settingsKey_;
    ReactionUsageModel model_ {16};
};

} // namespace Mattermost
