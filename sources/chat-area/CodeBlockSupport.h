#pragma once

#include <QString>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextFormat>
#include <QVector>

namespace Mattermost {

struct CodeBlockLanguageInfo {
    const char* id;
    const char* label;
};

inline const QVector<CodeBlockLanguageInfo>& codeBlockLanguages()
{
    static const QVector<CodeBlockLanguageInfo> languages {
        {"c", "C"},
        {"cpp", "C++"},
        {"csharp", "C#"},
        {"java", "Java"},
        {"js", "JavaScript"},
        {"typescript", "TypeScript"},
        {"python", "Python"},
        {"rust", "Rust"},
        {"go", "Go"},
        {"bash", "Bash / Shell"},
        {"php", "PHP"},
        {"qml", "QML"},
        {"sql", "SQL"},
        {"json", "JSON"},
        {"xml", "XML / HTML"},
        {"css", "CSS"},
        {"yaml", "YAML"},
        {"ini", "INI"},
        {"cmake", "CMake"},
        {"make", "Makefile"},
        {"asm", "Assembly"},
        {"lua", "Lua"},
        {"v", "V"},
        {"vex", "VEX"},
    };
    return languages;
}

inline QString canonicalCodeBlockLanguage(QString language)
{
    language = language.trimmed().toLower();
    const int whitespace = [&language] {
        const int space = language.indexOf(QLatin1Char(' '));
        const int tab = language.indexOf(QLatin1Char('\t'));
        if (space < 0) {
            return tab;
        }
        if (tab < 0) {
            return space;
        }
        return qMin(space, tab);
    }();
    if (whitespace >= 0) {
        language.truncate(whitespace);
    }

    if (language == QLatin1String("c++") || language == QLatin1String("cxx")
        || language == QLatin1String("cc")) {
        return QStringLiteral("cpp");
    }
    if (language == QLatin1String("javascript")) {
        return QStringLiteral("js");
    }
    if (language == QLatin1String("sh") || language == QLatin1String("shell")) {
        return QStringLiteral("bash");
    }
    if (language == QLatin1String("py")) {
        return QStringLiteral("python");
    }
    if (language == QLatin1String("rs")) {
        return QStringLiteral("rust");
    }
    if (language == QLatin1String("cs") || language == QLatin1String("c#")) {
        return QStringLiteral("csharp");
    }
    if (language == QLatin1String("html")) {
        return QStringLiteral("xml");
    }
    if (language == QLatin1String("ts")) {
        return QStringLiteral("typescript");
    }
    if (language == QLatin1String("yml")) {
        return QStringLiteral("yaml");
    }
    if (language == QLatin1String("makefile")) {
        return QStringLiteral("make");
    }
    if (language == QLatin1String("assembly")) {
        return QStringLiteral("asm");
    }

    for (const CodeBlockLanguageInfo& info : codeBlockLanguages()) {
        if (language == QLatin1String(info.id)) {
            return language;
        }
    }
    return {};
}

inline bool isStructuralCodeBlock(const QTextBlock& block)
{
    if (!block.isValid()) {
        return false;
    }

    const QTextBlockFormat format = block.blockFormat();
    // Qt may expose BlockCodeFence/BlockCodeLanguage as present properties with
    // empty/default values on non-code blocks. Presence alone is therefore not
    // a structural discriminator. A real code block has preformatted layout,
    // a non-empty fence, or an explicit language.
    return format.nonBreakableLines()
        || !format.stringProperty(QTextFormat::BlockCodeFence).isEmpty()
        || !format.stringProperty(QTextFormat::BlockCodeLanguage).isEmpty();
}

inline QString codeBlockLanguage(const QTextBlock& block)
{
    if (!block.isValid()) {
        return {};
    }
    return canonicalCodeBlockLanguage(
        block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage));
}

} // namespace Mattermost
