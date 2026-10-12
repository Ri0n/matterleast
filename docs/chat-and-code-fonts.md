# Chat and code-block fonts

Settings → Appearance → Chat controls the base body font (family and size).
Settings → Appearance → Code blocks independently selects a **monospaced
family** for fenced blocks. The code block's point size continues to follow the
chat font's point size. This preserves the previous scale behavior and avoids
tiny or oversized code when the chat font changes.

`chat/codeFontFamily` is a string option. If missing or empty, use the
platform's installed `QFontDatabase::FixedFont`, requested as a typewriter,
fixed-pitch font. A selection is previewed immediately, without restarting;
Cancel restores the prior value. `MessageContentWidget` rebuilds materialized
fenced blocks on option changes since each `CodeBlockEdit` owns a
`QPlainTextEdit` document with its own font.

Inline Markdown code spans deliberately retain the platform fixed font for
now. They use `QTextCharFormat` inside rich text, unlike the independent
fenced-block editors.
