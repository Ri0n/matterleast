#pragma once

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>

#include "chat-area/CodeBlockSupport.h"

namespace Mattermost {

/**
 * Paint presentation-only block decorations after QTextEdit has rendered its
 * viewport. The document itself remains canonical Markdown structure; this
 * helper draws only chrome that QTextDocument cannot represent directly
 * (quote bar and rounded code-block boundary).
 *
 * Keep the composer painter header-only: MessageTextEditWidget is also built by
 * a deliberately small unit-test target that does not link the full production
 * presentation object. The helper itself depends only on Qt text primitives.
 */
inline void paintComposerRichTextBlockDecorations(QTextEdit& editor,
                                                   QPainter& painter)
{
    if (!editor.viewport() || !editor.document()) {
        return;
    }

    const auto blockViewportRect = [&editor](const QTextBlock& block) {
        if (!block.isValid()) {
            return QRectF();
        }

        QTextCursor first(block);
        first.movePosition(QTextCursor::StartOfBlock);
        QTextCursor last(block);
        last.movePosition(QTextCursor::EndOfBlock);

        const QRect firstRect = editor.cursorRect(first);
        const QRect lastRect = editor.cursorRect(last);
        const qreal top = std::min(firstRect.top(), lastRect.top());
        const qreal bottom = std::max(firstRect.bottom(), lastRect.bottom());
        return QRectF(0.0, top, editor.viewport()->width(),
                      std::max<qreal>(1.0, bottom - top + 1.0));
    };

    const auto paintGroups = [&editor, &blockViewportRect](auto predicate,
                                                            auto paintGroup) {
        bool active = false;
        QRectF group;
        const auto flush = [&] {
            if (active && group.isValid() && group.bottom() >= 0
                && group.top() <= editor.viewport()->height()) {
                paintGroup(group);
            }
            active = false;
            group = {};
        };

        for (QTextBlock block = editor.document()->begin();
             block.isValid(); block = block.next()) {
            if (!predicate(block)) {
                flush();
                continue;
            }

            const QRectF current = blockViewportRect(block);
            if (!active) {
                active = true;
                group = current;
            } else {
                group = group.united(current);
            }
        }
        flush();
    };

    QColor codeBorder(227, 226, 214);
    codeBorder.setAlpha(88);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    paintGroups(
        [](const QTextBlock& block) { return isStructuralCodeBlock(block); },
        [&editor, &painter, &codeBorder](QRectF rect) {
            rect.setLeft(0.5);
            rect.setRight(std::max<qreal>(0.5,
                                         editor.viewport()->width() - 0.5));
            rect.adjust(0.0, -0.5, 0.0, 0.5);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(codeBorder, 1.0));
            painter.drawRoundedRect(rect, 4.0, 4.0);
        });

    paintGroups(
        [](const QTextBlock& block) {
            return block.isValid()
                && block.blockFormat().intProperty(
                       QTextFormat::BlockQuoteLevel) > 0;
        },
        [&editor, &painter](const QRectF& rect) {
            const QRectF barRect(
                1.0,
                rect.top(),
                3.0,
                std::max<qreal>(1.0, rect.height()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(editor.palette().color(QPalette::Mid));
            painter.drawRoundedRect(barRect, 1.5, 1.5);
        });

    painter.restore();
}

// Retained for the standalone presentation implementation. Composer painting
// uses the header-only helper above so isolated widget tests do not acquire a
// link dependency on RichTextBlockPresentation.cpp.
void paintRichTextBlockDecorations(QTextEdit& editor, QPainter& painter);

} // namespace Mattermost
