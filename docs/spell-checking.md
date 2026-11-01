# Spell checking

The message composer is a plain-Markdown `QTextEdit`. Spell checking never modifies its source text except when the user chooses a replacement.

## Architecture
- `SpellChecker` is the process-wide facade. A single platform backend is shared by all editors to avoid loading the Hunspell dictionaries per chat or thread.
- `SpellHighlighter` is one lightweight `QSyntaxHighlighter` per editor document. It underlines misspellings but does not touch the saved draft, clipboard text or outgoing posts.
- The source tokenizer handles Unicode letters and skips URLs, e-mail addresses, mentions, emoji shortcodes, inline code and Markdown fenced blocks. Offsets are UTF-16 document positions, consistent with QTextCursor.
- The right-click menu offers suggestions, session-only Ignore, and Add to MatterLeast dictionary. A replacement checks the original text again to avoid editing an unexpected range.

## Platforms
- Linux: libhunspell and installed MySpell/Hunspell dictionaries. Files are discovered under the common system directories and `MYSPELL_DICT_DIR`. Dictionary encodings are respected on Qt 5 and Qt 6. No network access or implicit dictionary downloads are performed. For example, install `hunspell-en-us` and/or `hunspell-ru` from your distribution. The build requires `hunspell-devel` or `libhunspell-dev`. CI and release packaging install the development dependency.
- Windows: Windows Spell Checking API via COM and languages installed in Windows.
- macOS: AppKit `NSSpellChecker`, using the system spell checking language preferences.
- Other targets: no backend is activated yet.

## Preferences
`MLOptions` owns all persistence:
- `composer/spellCheckEnabled`: enabled by default and toggled in the composer context menu.
- `composer/spellLanguages`: empty means default system locale plus English and Russian when available. Linux and Windows expose installed dictionaries in a context-menu submenu.
- `composer/spellCustomWords`: application-specific user additions; case-folded when matched. Ignore is session only.

When there is no usable dictionary/provider, the composer remains fully functional and no word is underlined. Linux bundles do not embed dictionaries; dictionaries must be installed by the user.
