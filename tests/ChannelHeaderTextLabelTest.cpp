#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QHelpEvent>
#include <QEvent>
#include <QSignalSpy>
#include <QToolTip>
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
#include <QTextFragment>
#include <QWidget>

#include "chat-area/ChannelHeaderTextLabel.h"
#include "chat-area/post/MessageContentWidget.h"

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
    void hoverOpensOnlyAfterTooltipDwell();
    void popoverIsAnimatedAndCapped();
    void popoverReverseKeepsRenderedChildren();
    void collapsedAndExpandedEmojiUseSameScale();
    void popoverUsesMessageCodeBlockRenderer();
    void collapsedMarkdownLinksKeepTargetTooltips();
    void topicLinksUsePostRendererAndNavigation();
    void collapsedLabelShowsHoveredLinkTooltip();
    void collapsedLabelClearsTooltipOnLeaveAndTextChange();
    void nativeWidthExpandsOnlyWhenTextDoesNotFit();
    void nativeWidthIncludesLabelMarginsAndIndent();
    void newlineExpandsEvenWhenTextFits();
    void replacingLongTopicWithShortTopicUpdatesOverflow();
};

void ChannelHeaderTextLabelTest::hoverOpensOnlyAfterTooltipDwell()
{
    QWidget host;
    host.resize(640, 300);
    ChannelHeaderTextLabel label(&host);
    label.setGeometry(20, 20, 400, label.sizeHint().height());
    label.setText(QStringLiteral("Topic opening line") + QLatin1Char(10)
                  + QStringLiteral("Long expanded topic details"));
    host.show();
    QTest::qWait(10);

    // Exercise the exact Enter/Leave transition methods without fabricating
    // QEvent::Enter: on Qt 6 QLabel expects a QEnterEvent carrying positions
    // and crashes if a bare QEvent is injected.
    QVERIFY(QMetaObject::invokeMethod(
        &label, "cancelPopoverShow", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &label, "schedulePopoverShow", Qt::DirectConnection));
    QTest::qWait(60);
    QVERIFY(!host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));

    // Brief accidental pointer crossing must not show the expanded topic.
    QVERIFY(QMetaObject::invokeMethod(
        &label, "cancelPopoverShow", Qt::DirectConnection));
    QTest::qWait(800);
    QVERIFY(!host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));

    QVERIFY(QMetaObject::invokeMethod(
        &label, "schedulePopoverShow", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(
        host.findChild<QScrollArea*>(
            QStringLiteral("channelHeaderTextPopover")) != nullptr,
        1200);
    QVERIFY(host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover"))->isVisible());
}

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

void ChannelHeaderTextLabelTest::collapsedMarkdownLinksKeepTargetTooltips()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    ChannelHeaderTextLabel label;
    const QString url = QStringLiteral("https://example.com/topic?a=1&b=2");
    label.setText(QStringLiteral("[open documentation](https://example.com/topic?a=1&b=2)"));

    // QLabel reparses the HTML, so check the actual document it will render.
    QTextDocument rendered;
    rendered.setHtml(label.text());
    bool foundLink = false;
    for (QTextBlock block = rendered.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().anchorHref() == url) {
                QCOMPARE(fragment.charFormat().toolTip(), url);
                foundLink = true;
            }
        }
    }
    QVERIFY(foundLink);
#else
    QSKIP("Markdown rendering test requires Qt 6.10");
#endif
}

void ChannelHeaderTextLabelTest::topicLinksUsePostRendererAndNavigation()
{
    const QString topic = QStringLiteral(
        "~offmarket ~off-pet-photo ~off-3d-printing "
        "[chat](https://mattermost.example.test/offtopic/channels/offmarket)");
    ChannelHeaderTextLabel detached;
    detached.setLinkTeamName(QStringLiteral("offtopic"));
    detached.setText(topic);

    QTextDocument rendered;
    rendered.setHtml(detached.text());
    for (const QString& slug : {QStringLiteral("offmarket"),
                                QStringLiteral("off-pet-photo"),
                                QStringLiteral("off-3d-printing")}) {
        QTextCursor ref = rendered.find(QLatin1Char('~') + slug);
        QVERIFY(!ref.isNull());
        ref.setPosition(ref.selectionStart() + 2);
        QCOMPARE(ref.charFormat().anchorHref(),
                 QStringLiteral("/offtopic/channels/") + slug);
    }

    QWidget host;
    host.resize(640, 300);
    ChannelHeaderTextLabel label(&host);
    label.setLinkTeamName(QStringLiteral("offtopic"));
    label.setText(topic);
    label.setGeometry(18, 16, 110, label.sizeHint().height());
    host.show();
    QVERIFY(showPopover(label));

    auto* content = host.findChild<MessageContentWidget*>(
        QStringLiteral("channelHeaderTextPopoverContent"));
    QVERIFY(content);
    const auto checkBrowser = [&] {
        auto* browser = content->findChild<QTextBrowser*>(
            QStringLiteral("messageRichText"));
        QVERIFY(browser);
        QVERIFY(!browser->openLinks());
        QVERIFY(!browser->openExternalLinks());
        QTextCursor ref = browser->document()->find(QStringLiteral("~offmarket"));
        QVERIFY(!ref.isNull());
        ref.setPosition(ref.selectionStart() + 2);
        QCOMPARE(ref.charFormat().anchorHref(),
                 QStringLiteral("/offtopic/channels/offmarket"));
    };
    checkBrowser();

    QUrl clicked;
    label.setLinkHandler([&clicked](const QUrl& url) { clicked = url; });
    const QUrl target(QStringLiteral(
        "https://mattermost.example.test/offtopic/channels/offmarket"));
    emit content->findChild<QTextBrowser*>(
        QStringLiteral("messageRichText"))->anchorClicked(target);
    QCOMPARE(clicked, target);

    // Palette/content rebuilds replace QTextBrowser: the same link handler
    // must be reconnected without letting Qt open its own browser.
    content->setMessage(topic);
    checkBrowser();
    clicked = QUrl();
    emit content->findChild<QTextBrowser*>(
        QStringLiteral("messageRichText"))->anchorClicked(target);
    QCOMPARE(clicked, target);
}

void ChannelHeaderTextLabelTest::collapsedLabelShowsHoveredLinkTooltip()
{
    QWidget host;
    host.resize(800, 240);
    ChannelHeaderTextLabel label(&host);
    const QString url = QStringLiteral("https://example.com/docs?x=1&y=2");

    // Verify the full Markdown-to-QLabel path on the modern renderer.
    // Older Qt versions use an explicit HTML anchor to exercise QLabel's
    // hover implementation independently of their Markdown feature set.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    label.setText(QStringLiteral(
        "[Documentation](https://example.com/docs?x=1&y=2) and plain text"));
#else
    label.QLabel::setText(
        QStringLiteral("<a href=\"https://example.com/docs?x=1&amp;y=2\" "
                       "title=\"https://example.com/docs?x=1&amp;y=2\">"
                       "Documentation</a> and plain text"));
#endif
    label.setGeometry(20, 20, 500, label.sizeHint().height());
    QSignalSpy hoverSpy(&label, &QLabel::linkHovered);

    host.show();
    QTest::qWait(10);

    const QPoint linkPoint(18, label.height() / 2);
    QTest::mouseMove(&label, linkPoint);
    QTRY_COMPARE(label.toolTip(), url);
    QVERIFY(hoverSpy.count() >= 1);

    // Prove the actual widget tooltip path works, not just that the HTML
    // contains title attributes. A QLabel without the hover bridge fails.
    QHelpEvent help(QEvent::ToolTip, linkPoint, label.mapToGlobal(linkPoint));
    QApplication::sendEvent(&label, &help);
    QTRY_COMPARE(QToolTip::text(), url);
    QToolTip::hideText();

    QTest::mouseMove(&label, QPoint(480, label.height() / 2));
    QTRY_VERIFY(label.toolTip().isEmpty());
}

void ChannelHeaderTextLabelTest::collapsedLabelClearsTooltipOnLeaveAndTextChange()
{
    QWidget host;
    host.resize(800, 240);
    ChannelHeaderTextLabel label(&host);
    label.setGeometry(20, 20, 500, label.sizeHint().height());
    host.show();
    QTest::qWait(10);

    const QString url = QStringLiteral("https://example.com/first");
    // QLabel does not always re-emit linkHovered when returning straight to
    // the same link after a Leave. The widget must restore the URL on Enter
    // even if QLabel's private text control suppresses a duplicate signal.
    label.QLabel::setText(QStringLiteral(
        "<a href=\"https://example.com/first\" "
        "title=\"https://example.com/first\">Link</a>"));
    const QPoint linkPoint(12, label.height() / 2);
    QTest::mouseMove(&label, linkPoint);
    QTRY_COMPARE(label.toolTip(), url);

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&label, &leave);
    QVERIFY(label.toolTip().isEmpty());

    // Re-enter directly over the same link, without first moving across
    // non-link text. A real window system sends Enter before MouseMove.
    // QTest::mouseMove() alone does not synthesize Enter on every QPA.
    QEnterEvent enter(linkPoint, linkPoint, label.mapToGlobal(linkPoint));
    QApplication::sendEvent(&label, &enter);
    QTest::mouseMove(&label, linkPoint);
    QTRY_COMPARE(label.toolTip(), url);
    QHelpEvent help(QEvent::ToolTip, linkPoint, label.mapToGlobal(linkPoint));
    QApplication::sendEvent(&label, &help);
    QTRY_COMPARE(QToolTip::text(), url);
    QToolTip::hideText();

    // Non-link text is covered independently by
    // collapsedLabelShowsHoveredLinkTooltip(). A synthetic Leave+Enter on
    // the offscreen QPA is not a real pointer transition, so do not rely
    // on its subsequent unrelated mouse-move state.

    // Switching channels also clears any old URL tooltip.
    label.setText(QStringLiteral("Another channel topic"));
    QVERIFY(label.toolTip().isEmpty());
}

void ChannelHeaderTextLabelTest::nativeWidthExpandsOnlyWhenTextDoesNotFit()
{
    QWidget host;
    host.resize(1200, 300);

    ChannelHeaderTextLabel label(&host);
    label.setText(QStringLiteral(
        "[Documentation with a longer link label](https://example.com/docs)"));
    const int naturalWidth = label.QLabel::sizeHint().width();
    QVERIFY(naturalWidth > 30);

    // The header must not make its parent pane wider just to fit the topic.
    QCOMPARE(label.sizeHint().width(), 0);
    QCOMPARE(label.minimumSizeHint().width(), 0);

    label.setGeometry(20, 20, naturalWidth + 16, label.sizeHint().height());
    host.show();

    QVERIFY(showPopover(label));
    QVERIFY(!host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));

    // Width is measured by QLabel's already parsed rich-text document.
    label.setFixedWidth(naturalWidth - 12);
    QVERIFY(showPopover(label));
    auto* popover = host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover"));
    QVERIFY(popover);
    QVERIFY(popover->isVisible());
}

void ChannelHeaderTextLabelTest::nativeWidthIncludesLabelMarginsAndIndent()
{
    QWidget host;
    host.resize(1200, 300);

    ChannelHeaderTextLabel label(&host);
    label.setMargin(7);
    label.setIndent(11);
    label.setText(QStringLiteral("Topic with **bold** and *italic* text"));
    const int naturalWidth = label.QLabel::sizeHint().width();
    QVERIFY(naturalWidth > 40);

    label.setGeometry(20, 20, naturalWidth + 16, label.sizeHint().height());
    host.show();

    // Avoid false expansion from the header's non-zero native insets.
    QVERIFY(showPopover(label));
    QVERIFY(!host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));

    label.setFixedWidth(naturalWidth - 12);
    QVERIFY(showPopover(label));
    QVERIFY(host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));
}

void ChannelHeaderTextLabelTest::newlineExpandsEvenWhenTextFits()
{
    QWidget host;
    host.resize(1200, 300);

    ChannelHeaderTextLabel label(&host);
    label.setText(QStringLiteral("first line\nsecond line"));
    const int naturalWidth = label.QLabel::sizeHint().width();
    label.setGeometry(20, 20, naturalWidth + 100, label.sizeHint().height());
    host.show();

    // A topic with two source lines needs expansion, regardless of its width.
    QVERIFY(showPopover(label));
    auto* popover = host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover"));
    QVERIFY(popover);
    QVERIFY(popover->isVisible());
}

void ChannelHeaderTextLabelTest::replacingLongTopicWithShortTopicUpdatesOverflow()
{
    QWidget host;
    host.resize(1200, 300);

    ChannelHeaderTextLabel label(&host);
    label.setText(QStringLiteral("A very long header topic with several words"));
    const int longWidth = label.QLabel::sizeHint().width();
    QVERIFY(longWidth > 100);
    label.setGeometry(20, 20, longWidth - 30, label.sizeHint().height());
    host.show();

    // The cached QLabel layout must be updated when the topic changes.
    label.setText(QStringLiteral("ok"));
    QVERIFY(label.QLabel::sizeHint().width() < label.width() - 4);
    QVERIFY(showPopover(label));
    QVERIFY(!host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));

    label.setText(QStringLiteral("A very long header topic with several words"));
    QVERIFY(showPopover(label));
    QVERIFY(host.findChild<QScrollArea*>(
        QStringLiteral("channelHeaderTextPopover")));
}

QTEST_MAIN(ChannelHeaderTextLabelTest)
#include "ChannelHeaderTextLabelTest.moc"
