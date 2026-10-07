#pragma once

#include <QStringList>

class QPushButton;

namespace Mattermost {
class EmojiRegistry;
namespace RankedEmojiPresentation {

/**
 * Return ranked names that are currently renderable.
 *
 * Looking up an unresolved custom name may trigger the shared lazy custom-emoji
 * resolver, but this helper never enumerates the custom-emoji catalog.
 */
QStringList renderableNames(EmojiRegistry& registry, const QStringList& names);

/**
 * Apply the shared built-in/custom emoji presentation to a compact button.
 * The caller owns the action-specific accessible name.
 */
bool configureButton(EmojiRegistry& registry, QPushButton& button,
                     const QString& name);

} // namespace RankedEmojiPresentation
} // namespace Mattermost
