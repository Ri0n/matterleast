#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>

#include "chat-area/CodeBlockSupport.h"
#include "chat-area/outgoing-post/MessageTextEditWidget.h"
#include "chat-area/outgoing-post/RichTextEditorCommands.h"

namespace Mattermost {
namespace {

constexpr char LanguageMenuInjectedProperty[] =
    "_matterleast_code_language_menu_injected";

bool hasStandardShortcut(const QMenu& menu, QKeySequence::StandardKey key)
{
    const QKeySequence expected(key);
    for (QAction* action : menu.actions()) {
        if (!action) {
            continue;
        }
        const QList<QKeySequence> shortcuts = action->shortcuts();
        for (const QKeySequence& shortcut : shortcuts) {
            if (shortcut.matches(expected) == QKeySequence::ExactMatch) {
                return true;
            }
        }
    }
    return false;
}

bool isStandardTextContextMenu(const QMenu& menu)
{
    // OutgoingPostCreator also parents other QMenus (notably message priority)
    // to itself. Requiring the QTextEdit Copy/Paste actions keeps language
    // injection scoped to createStandardContextMenu() rather than every menu
    // opened while the composer owns focus.
    return hasStandardShortcut(menu, QKeySequence::Copy)
        && hasStandardShortcut(menu, QKeySequence::Paste);
}

class CodeBlockLanguageMenuFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event || event->type() != QEvent::Show) {
            return false;
        }

        auto* menu = qobject_cast<QMenu*>(watched);
        if (!menu || menu->property(LanguageMenuInjectedProperty).toBool()
            || !isStandardTextContextMenu(*menu)) {
            return false;
        }

        // createStandardContextMenu() keeps the editor as the menu parent.
        auto* editor = qobject_cast<MessageTextEditWidget*>(menu->parentWidget());
        if (!editor || !editor->isRichTextEditing()
            || !isStructuralCodeBlock(editor->textCursor().block())) {
            return false;
        }

        menu->setProperty(LanguageMenuInjectedProperty, true);
        menu->addSeparator();
        QMenu* languageMenu = menu->addMenu(editor->tr("Code language"));

        auto* group = new QActionGroup(languageMenu);
        group->setExclusive(true);
        const QString current =
            RichTextEditorCommands::codeBlockLanguageAt(*editor);
        QPointer<MessageTextEditWidget> guard(editor);

        QAction* plain = languageMenu->addAction(editor->tr("Plain text"));
        plain->setCheckable(true);
        plain->setChecked(current.isEmpty());
        group->addAction(plain);
        QObject::connect(plain, &QAction::triggered, editor, [guard] {
            if (guard) {
                RichTextEditorCommands::setCodeBlockLanguage(*guard, QString());
            }
        });

        languageMenu->addSeparator();
        for (const CodeBlockLanguageInfo& info : codeBlockLanguages()) {
            const QString id = QString::fromLatin1(info.id);
            QAction* action = languageMenu->addAction(
                QCoreApplication::translate("CodeBlockLanguage", info.label));
            action->setCheckable(true);
            action->setChecked(current == id);
            action->setData(id);
            group->addAction(action);
            QObject::connect(action, &QAction::triggered, editor,
                             [guard, id] {
                if (guard) {
                    RichTextEditorCommands::setCodeBlockLanguage(*guard, id);
                }
            });
        }

        return false;
    }
};

void installCodeBlockLanguageMenu()
{
    if (!qApp) {
        return;
    }
    qApp->installEventFilter(new CodeBlockLanguageMenuFilter(qApp));
}

} // namespace
} // namespace Mattermost

static void installMatterLeastCodeBlockLanguageMenu()
{
    Mattermost::installCodeBlockLanguageMenu();
}

Q_COREAPP_STARTUP_FUNCTION(installMatterLeastCodeBlockLanguageMenu)
