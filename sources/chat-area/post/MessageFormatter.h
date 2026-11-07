#pragma once

#include <QColor>
#include <QString>
#include <QtGlobal>

class QTextDocument;

namespace Mattermost {

class EmojiRegistry;
namespace MessageFormatter {

QString formatMessageText(const QString& text, EmojiRegistry* registry = nullptr);
// Restore URL tooltips after Qt's QTextDocument -> HTML serialization.
QString htmlWithLinkToolTips(const QString& html);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
void buildMarkdownDocument(QTextDocument& document, const QString& text,
                           EmojiRegistry* registry = nullptr,
                           const QColor& inlineCodeTextColor = QColor());
#endif

} // namespace MessageFormatter
} // namespace Mattermost
