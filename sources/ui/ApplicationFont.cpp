#include "ApplicationFont.h"

#include <QApplication>
#include <QFont>

#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost {

void initializeApplicationFont(QApplication& app)
{
    const QFont systemFont = app.font();
    QFont defaultFont = systemFont;
    defaultFont.setPointSize(11);
    auto* options = MLOptions::instance();
    // New keys win over legacy settings, including after a preview rollback.
    if (!options->contains(CHAT_FONT) && options->contains(QStringLiteral("chat/font"))) {
        options->setValue<QString>(CHAT_FONT,
            options->value<QString>(QStringLiteral("chat/font")));
    }
    if (!options->contains(INTERFACE_FONT)) {
        const int size = options->value<int>(QStringLiteral("ui/fontPointSize"), 0);
        if (size > 0) {
            QFont migrated = systemFont;
            migrated.setPointSize(qBound(8, size, 30));
            options->setValue<QString>(INTERFACE_FONT, migrated.toString());
        }
    }
    // Register chat's independent default before any widget can derive it
    // from an already customized interface font.
    options->optionObject<QString>(CHAT_FONT, defaultFont.toString());
    auto* option = options->optionObject<QString>(INTERFACE_FONT, QString());
    const auto apply = [defaultFont](const QVariant& value) {
        QFont font = defaultFont;
        QFont parsed;
        const QString serialized = value.toString();
        if (!serialized.isEmpty() && parsed.fromString(serialized)) font = parsed;
        QApplication::setFont(font);
    };
    QObject::connect(option, &MLOptionObject::changed, &app, apply);
    apply(option->value());
}

} // namespace Mattermost
