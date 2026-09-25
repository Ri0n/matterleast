#include "RankedEmojiPresentation.h"

#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>

#include "backend/emoji/EmojiInfo.h"
#include "ui/EmojiPresentation.h"

namespace Mattermost::RankedEmojiPresentation {
namespace {

QString customEmojiSource(const QString& presentation)
{
    static const QRegularExpression sourceExpression(
        QStringLiteral(R"(\bsrc\s*=\s*["']([^"']+)["'])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = sourceExpression.match(presentation);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString pixmapPath(QString source)
{
    if (source.startsWith(QStringLiteral("qrc://"))) {
        source = QStringLiteral(":/") + source.mid(6);
    }
    return EmojiPresentation::imagePath(source);
}

bool isRenderable(const QString& name)
{
    const EmojiID id = EmojiInfo::findByName(name);
    if (!id) {
        return false;
    }

    const Emoji emoji = EmojiInfo::getEmoji(id);
    const QString source = customEmojiSource(emoji.unicodeString);
    if (source.isEmpty()) {
        return !emoji.unicodeString.trimmed().isEmpty();
    }
    return !QPixmap(pixmapPath(source)).isNull();
}

} // namespace

QStringList renderableNames(const QStringList& names)
{
    QStringList result;
    result.reserve(names.size());
    for (const QString& name : names) {
        if (isRenderable(name)) {
            result.push_back(name);
        }
    }
    return result;
}

bool configureButton(QPushButton& button, const QString& name)
{
    const EmojiID id = EmojiInfo::findByName(name);
    if (!id) {
        return false;
    }

    const Emoji emoji = EmojiInfo::getEmoji(id);
    const QString source = customEmojiSource(emoji.unicodeString);
    if (!source.isEmpty()) {
        const QPixmap pixmap(pixmapPath(source));
        if (pixmap.isNull()) {
            return false;
        }
        button.setIcon(QIcon(pixmap));
        button.setIconSize(QSize(20, 20));
    } else {
        const QString text = emoji.unicodeString.trimmed();
        if (text.isEmpty()) {
            return false;
        }
        button.setText(text);
        button.setFont(EmojiPresentation::fontForMode(
            button.font(), EmojiPresentation::Mode::Reaction));
    }

    button.setToolTip(QStringLiteral(":%1:").arg(name));
    return true;
}

} // namespace Mattermost::RankedEmojiPresentation
