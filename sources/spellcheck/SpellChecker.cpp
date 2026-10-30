#include "SpellChecker.h"
#include <QLocale>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVector>
#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {
QList<SpellingWord> wordsInLine(const QString& line)
{
    QVector<bool> excluded(line.size(), false);
    const auto mask = [&excluded, &line](int begin, int length) {
        const int end = qMin(begin + length, line.size());
        for (int i = qMax(0, begin); i < end; ++i)
            excluded[i] = true;
    };
    static const QRegularExpression opaque(
        QStringLiteral(R"((?:[a-zA-Z][a-zA-Z0-9+.-]*://|www\.)[^\s<>()]+|[\p{L}\p{N}._%+-]+@[\p{L}\p{N}.-]+\.[\p{L}]{2,}|:[\p{L}\p{N}_+-]+:|@[\p{L}\p{N}_-]+)"),
        QRegularExpression::UseUnicodePropertiesOption);
    auto skips = opaque.globalMatch(line);
    while (skips.hasNext()) {
        const auto match = skips.next();
        mask(match.capturedStart(), match.capturedLength());
    }
    // Markdown inline code must not be checked, including its delimiters.
    for (int start = 0; start < line.size();) {
        if (line.at(start) != QLatin1Char('`')) {
            ++start;
            continue;
        }
        int count = 1;
        while (start + count < line.size()
               && line.at(start + count) == QLatin1Char('`'))
            ++count;
        const QString delimiter(count, QLatin1Char('`'));
        const int end = line.indexOf(delimiter, start + count);
        mask(start, (end < 0 ? line.size() : end + count) - start);
        start = end < 0 ? line.size() : end + count;
    }
    static const QRegularExpression expression(
        QStringLiteral(R"([\p{L}][\p{L}\p{M}'’\-]*[\p{L}\p{M}]|[\p{L}])"),
        QRegularExpression::UseUnicodePropertiesOption);
    QList<SpellingWord> words;
    auto it = expression.globalMatch(line);
    while (it.hasNext()) {
        const auto match = it.next();
        const int begin = match.capturedStart();
        const int length = match.capturedLength();
        if (length < 2)
            continue;
        bool ignore = false;
        for (int i = begin; i < begin + length; ++i) {
            if (excluded.at(i)) {
                ignore = true;
                break;
            }
        }
        if (!ignore)
            words.append({begin, length, match.captured()});
    }
    return words;
}
QStringList defaultLanguages()
{
    QStringList result { QLocale::system().name(),
                         QStringLiteral("en_US"),
                         QStringLiteral("ru_RU") };
    result.removeDuplicates();
    return result;
}
} // namespace
QList<SpellingWord> spellingWords(const QString& line) { return wordsInLine(line); }
SpellingWord spellingWordAt(const QString& line, int position)
{
    for (const auto& word : wordsInLine(line)) {
        if (position >= word.start && position < word.start + word.length)
            return word;
    }
    return {};
}
SpellChecker& SpellChecker::instance()
{
    static SpellChecker checker;
    return checker;
}
SpellChecker::SpellChecker() : _backend(createPlatformSpellBackend())
{
    auto* opts = MLOptions::instance();
    for (const QString& word : opts->value<QStringList>(SPELLCHECK_CUSTOM_WORDS))
        _customWords.insert(word.toCaseFolded());
    configureLanguages();
    connect(opts->optionObject<QStringList>(SPELLCHECK_LANGUAGES),
            &MLOptionObject::changed, this, [this](const QVariant&) {
        configureLanguages();
        emit changed();
    });
}
void SpellChecker::configureLanguages()
{
    if (!_backend)
        return;
    const QStringList selected =
        MLOptions::instance()->value<QStringList>(SPELLCHECK_LANGUAGES);
    _backend->configure(selected.isEmpty() ? defaultLanguages() : selected);
}
bool SpellChecker::available() const { return _backend && _backend->available(); }
bool SpellChecker::correct(const QString& word) const
{
    const QString folded = word.toCaseFolded();
    return !available() || _customWords.contains(folded)
        || _sessionWords.contains(folded) || _backend->correct(word);
}
QStringList SpellChecker::suggestions(const QString& word) const
{
    return available() ? _backend->suggestions(word) : QStringList();
}
QStringList SpellChecker::languages() const
{
    return _backend ? _backend->languages() : QStringList();
}
QStringList SpellChecker::selectedLanguages() const
{
    const QStringList saved =
        MLOptions::instance()->value<QStringList>(SPELLCHECK_LANGUAGES);
    return saved.isEmpty() ? defaultLanguages() : saved;
}
void SpellChecker::setSelectedLanguages(const QStringList& languages)
{
    if (languages.isEmpty())
        return;
    MLOptions::instance()->optionObject<QStringList>(SPELLCHECK_LANGUAGES)
        ->setValue(languages);
}
void SpellChecker::ignore(const QString& word)
{
    _sessionWords.insert(word.toCaseFolded());
    emit changed();
}
void SpellChecker::addToDictionary(const QString& word)
{
    if (word.isEmpty())
        return;
    const QString folded = word.toCaseFolded();
    if (_customWords.contains(folded))
        return;
    _customWords.insert(folded);
    QStringList saved =
        MLOptions::instance()->value<QStringList>(SPELLCHECK_CUSTOM_WORDS);
    saved.append(word);
    MLOptions::instance()->setValue<QStringList>(SPELLCHECK_CUSTOM_WORDS, saved);
    emit changed();
}
SpellHighlighter::SpellHighlighter(QTextDocument* document)
    : QSyntaxHighlighter(document)
{
    connect(&SpellChecker::instance(), &SpellChecker::changed,
            this, [this] { rehighlight(); });
}
void SpellHighlighter::highlightBlock(const QString& line)
{
    const QString trimmed = line.trimmed();
    const bool fence = trimmed.startsWith(QStringLiteral("```"))
        || trimmed.startsWith(QStringLiteral("~~~"));
    const bool inside = previousBlockState() == 1;
    if (fence) {
        setCurrentBlockState(inside ? 0 : 1);
        return;
    }
    if (inside) {
        setCurrentBlockState(1);
        return;
    }
    setCurrentBlockState(0);
    if (!SpellChecker::instance().available())
        return;
    QTextCharFormat format;
    format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    format.setUnderlineColor(QColor(210, 48, 64));
    for (const auto& word : spellingWords(line)) {
        if (!SpellChecker::instance().correct(word.text))
            setFormat(word.start, word.length, format);
    }
}
} // namespace Mattermost
