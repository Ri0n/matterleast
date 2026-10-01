#pragma once

class QPainter;
class QTextEdit;

namespace Mattermost {

/**
 * Paint presentation-only block decorations after QTextEdit has rendered its
 * viewport. The document itself remains canonical Markdown structure; this
 * helper draws only chrome that QTextDocument cannot represent directly
 * (quote bar and rounded code-block boundary).
 */
void paintRichTextBlockDecorations(QTextEdit& editor, QPainter& painter);

} // namespace Mattermost
