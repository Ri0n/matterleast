#pragma once

#include <functional>

#include <QSet>
#include <QString>

class QObject;
class QTextDocument;

namespace Mattermost {
class Backend;

// Completes the same team-scoped ~channel links used by PostWidget and topics:
// the anchor keeps the channel slug URL while its visible text is replaced
// with the server's display_name, even for channels outside the sidebar.
namespace ChannelReferenceDisplayNames {

QSet<QString> unresolvedSlugs(const QTextDocument& document,
                              const QString& teamName);
bool replace(QTextDocument& document, const QString& teamName,
             const QString& slug, const QString& displayName);
void resolve(QTextDocument& document, Backend& backend,
             const QString& teamName, QObject* owner,
             std::function<void()> onChanged = {});

} // namespace ChannelReferenceDisplayNames
} // namespace Mattermost
