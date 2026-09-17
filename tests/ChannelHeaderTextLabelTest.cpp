#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QLayout>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

#include "chat-area/ChannelHeaderTextLabel.h"

using namespace Mattermost;

namespace {

bool showPopover(ChannelHeaderTextLabel& label)
{
    return QMetaObject::invokeMethod(&label, "showPopover", Qt::DirectConnection);
}

bool hidePopover(ChannelHeaderTextLabel& label)
{
    return QMetaObject::invokeMethod(&label, "hidePopover", Qt::DirectConnection);
}

} // namespace

class ChannelHeaderTextLabelTest : public QObject
{
    Q_OBJECT

private slots:
    void popoverIsAnimatedAndCapped();
    void popoverReverseKeepsRenderedChildren();
    void collapsedAndExpandedEmojiUseSameScale();
    void popoverUsesMessageCodeBlockRenderer();
};

void ChannelHeaderTextLabelTest::popoverIsAnimatedAndCapped()
{
    QWidget host;
    host.resize(640, 400);

    ChannelHeaderTextLabel label(&host);
    label.setStyleSheet(QStringLiteral("padding-left: 1px; padding-bottom: 3px"));
    // QLabel paints rich text after both its margin and indent. Keep these
    // non-zero so the test catches alignment against contentsRect() alone.
    label.setMargin(2);
    label.setIndent(3);
    label.setGeometry(64, 20, host.width() - 64, label.sizeHint().height());
    label.setText(QStringLiteral("first line\n")
                      + QStringLiteral("another fairly long topic line\n").repeated(40));

    host.show();
    QTest::qWait(10);

    QVERIFY(showPopover(label));

    auto* popover = host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover"));
    QVERIFY(popover);
    QVERIFY(popover->isVisible());
    auto* content = host.findChild<QWidget*>(
        QStringLiteral("channelHeaderTextPopoverContent"));
    auto* container = host.findChild<QWidget*>(
        QStringLiteral("channelHeaderTextPopoverContainer"));
    QVERIFY(content);
    QVERIFY(container);

    const int collapsedTextLeft = label.mapTo(
        &host,
        QPoint(label.contentsRect().left() + label.margin() + label.indent(), 0)).x();
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        // MessageContentWidget can rebuild its rich-text child while deferred
        // layout settles. Reacquire it on every retry rather than keeping a raw
        // pointer across QTRY's event-loop iterations.
        auto* richText = content->findChild<QTextBrowser*>(
            QStringLiteral("messageRichText"));
        if (!richText) {
            return false;
        }
        const QTextBlock firstBlock = richText->document()->firstBlock();
        if (!firstBlock.isValid() || !richText->document()->documentLayout()) {
            return false;
        }
        const int blockLeft = static_cast<int>(std::lround(
            richText->document()->documentLayout()->blockBoundingRect(firstBlock).left()));
        const int expandedTextLeft = richText->viewport()
                                         ->mapTo(&host, QPoint(0, 0))
                                         .x()
            + blockLeft;
        return expandedTextLeft == collapsedTextLeft;
    }(), 1000);
    QVERIFY(container->layout());
    const QMargins contentMargins = container->layout()->contentsMargins();
    QCOMPARE(contentMargins.top(), 0);
    QCOMPARE(contentMargins.right(), 16);
    QCOMPARE(contentMargins.bottom(), 8);

    auto* animation = label.findChild<QPropertyAnimation*>(
        QStringLiteral("channelHeaderTextPopoverAnimation"));
    QVERIFY(animation);
    QCOMPARE(animation->duration(), 180);

    QTRY_VERIFY_WITH_TIMEOUT(animation->state() == QAbstractAnimation::Stopped, 1000);
    QVERIFY(popover->height() <= host.height() / 2);
    QTRY_VERIFY_WITH_TIMEOUT(popover->verticalScrollBar()->maximum() > 0, 1000);
    QVERIFY(container->height() - (content->y() + content->height()) >= 8);
}

void ChannelHeaderTextLabelTest::popoverReverseKeepsRenderedChildren()
{
    QWidget host;
    host.resize(640, 400);

    ChannelHeaderTextLabel label(&host);
    label.setGeometry(32, 20, host.width() - 32, label.sizeHint().height());
    label.setText(QStringLiteral("first line\nsecond line\nthird line"));

    host.show();
    QTest::qWait(10);

    QVERIFY(showPopover(label));

    auto* popover = host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover"));
    QVERIFY(popover);
    auto* content = host.findChild<QWidget*>(
        QStringLiteral("channelHeaderTextPopoverContent"));
    QVERIFY(content);
    auto* animation = label.findChild<QPropertyAnimation*>(
        QStringLiteral("channelHeaderTextPopoverAnimation"));
    QVERIFY(animation);

    QTRY_VERIFY_WITH_TIMEOUT(
        content->findChild<QTextBrowser*>(QStringLiteral("messageRichText")) != nullptr,
        1000);
    QPointer<QTextBrowser> original = content->findChild<QTextBrowser*>(
        QStringLiteral("messageRichText"));
    QVERIFY(original);

    QTRY_VERIFY_WITH_TIMEOUT(animation->state() == QAbstractAnimation::Stopped, 1000);

    QVERIFY(hidePopover(label));
    QTRY_VERIFY_WITH_TIMEOUT(animation->state() == QAbstractAnimation::Running, 500);

    QVERIFY(showPopover(label));

    QVERIFY(original);
    QCOMPARE(content->findChild<QTextBrowser*>(QStringLiteral("messageRichText")),
             original.data());
    QVERIFY(popover->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(animation->state() == QAbstractAnimation::Stopped, 1000);
    QVERIFY(popover->isVisible());
}

void ChannelHeaderTextLabelTest::collapsedAndExpandedEmojiUseSameScale()
{
    QWidget host;
    host.resize(640, 400);

    ChannelHeaderTextLabel label(&host);
    label.setGeometry(32, 20, host.width() - 32, label.sizeHint().height());
    label.setText(QStringLiteral("topic 😀\nsecond line"));

    host.show();
    QTest::qWait(10);

    QTextDocument collapsedDocument;
    collapsedDocument.setDefaultFont(label.font());
    collapsedDocument.setHtml(label.text());
    const int collapsedEmojiPosition = collapsedDocument.toPlainText().indexOf(
        QStringLiteral("😀"));
    QVERIFY(collapsedEmojiPosition >= 0);
    QTextCursor collapsedCursor(&collapsedDocument);
    collapsedCursor.setPosition(collapsedEmojiPosition);
    qreal collapsedPointSize = collapsedCursor.charFormat().fontPointSize();
    if (collapsedPointSize <= 0.0) {
        collapsedPointSize = collapsedDocument.defaultFont().pointSizeF();
    }
    QVERIFY(collapsedPointSize > 0.0);

    QVERIFY(showPopover(label));

    auto* content = host.findChild<QWidget*>(
        QStringLiteral("channelHeaderTextPopoverContent"));
    QVERIFY(content);
    QTRY_VERIFY_WITH_TIMEOUT(
        content->findChild<QTextBrowser*>(QStringLiteral("messageRichText")) != nullptr,
        1000);
    QPointer<QTextBrowser> richText = content->findChild<QTextBrowser*>(
        QStringLiteral("messageRichText"));
    QVERIFY(richText);

    const int expandedEmojiPosition = richText->document()->toPlainText().indexOf(
        QStringLiteral("😀"));
    QVERIFY(expandedEmojiPosition >= 0);
    QTextCursor expandedCursor(richText->document());
    expandedCursor.setPosition(expandedEmojiPosition);
    qreal expandedPointSize = expandedCursor.charFormat().fontPointSize();
    if (expandedPointSize <= 0.0) {
        expandedPointSize = richText->document()->defaultFont().pointSizeF();
    }
    QVERIFY(expandedPointSize > 0.0);

    QVERIFY(std::abs(collapsedPointSize - expandedPointSize) < 0.01);
}

void ChannelHeaderTextLabelTest::popoverUsesMessageCodeBlockRenderer()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    QWidget host;
    host.resize(640, 400);

    ChannelHeaderTextLabel label(&host);
    label.setGeometry(0, 20, host.width(), label.sizeHint().height());
    label.setText(QStringLiteral(
        "topic before code\n```cpp\nint answer = 42;\n```\ntopic after code"));

    host.show();
    QTest::qWait(10);

    QVERIFY(showPopover(label));

    QTRY_VERIFY_WITH_TIMEOUT(
        host.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock")) != nullptr,
        1000);
#else
    QSKIP("Dedicated fenced-code widgets require Qt 6.10 Markdown support");
#endif
}

QTEST_MAIN(ChannelHeaderTextLabelTest)
#include "ChannelHeaderTextLabelTest.moc"
