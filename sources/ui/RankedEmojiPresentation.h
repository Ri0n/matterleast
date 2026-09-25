#pragma once

#include <QStringList>

class QPushButton;

namespace Mattermost::RankedEmojiPresentation {

/**
 * Return ranked names that are currently renderable.
 *
 * Looking up an unresolved custom name may trigger the shared lazy custom-emoji
 * resolver, but this helper never enumerates the custom-emoji catalog.
 */
QStringList renderableNames(const QStringList& names);

/**
 * Apply the shared built-in/custom emoji presentation to a compact button.
 * The caller owns the action-specific accessible name.
 */
bool configureButton(QPushButton& button, const QString& name);

} // namespace Mattermost::RankedEmojiPresentation
