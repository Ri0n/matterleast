#include <QtTest>

#include "chat-area/outgoing-post/ComposerPreviewMarkdown.h"

using namespace Mattermost;

class ComposerPreviewMarkdownTest : public QObject
{
    Q_OBJECT

private slots:
    void inlineImagesResolveToLocalFiles()
    {
        const OutgoingAttachmentItem item {
            QStringLiteral("aabbccdd-1234-1234-1234-112233445566"),
            QStringLiteral("/tmp/diagram.png")
        };
        const QString markdown = QStringLiteral("See ![diagram](")
            + LocalAttachmentMarkdown::uri(item.id, 0)
            + QStringLiteral(") and **bold**");
        const auto prepared = ComposerPreviewMarkdown::prepare(markdown, {item});
        QCOMPARE(prepared.markdown,
                 QStringLiteral("See ![diagram](/api/v4/files/")
                     + item.id + QStringLiteral(") and **bold**"));
        QCOMPARE(prepared.imagePaths.value(item.id), item.path);
        QCOMPARE(prepared.markdown.count(QStringLiteral("![")), 1);
    }

    void pastedClipboardImagesAppearWithoutInlineMarkdown()
    {
        const OutgoingAttachmentItem image {
            QStringLiteral("bbccddaa-1234-1234-1234-112233445566"),
            QStringLiteral("/tmp/mattermost-qt-clipboard/clipboard-image.png")
        };
        const OutgoingAttachmentItem document {
            QStringLiteral("ccddeeff-1234-1234-1234-112233445566"),
            QStringLiteral("/tmp/document.pdf")
        };
        const auto prepared = ComposerPreviewMarkdown::prepare(
            QStringLiteral("Hello"), {image, document});
        QCOMPARE(prepared.markdown,
                 QStringLiteral("Hello\n\n![clipboard-image.png](/api/v4/files/")
                     + image.id + QStringLiteral(")"));
        QCOMPARE(prepared.imagePaths.size(), 1);
        QVERIFY(!prepared.imagePaths.contains(document.id));
    }

    void removedAttachmentsCannotBeResolved()
    {
        const QString orphan = LocalAttachmentMarkdown::image(
            QStringLiteral("missing"), QStringLiteral("abbc-1234"), 0);
        const auto prepared = ComposerPreviewMarkdown::prepare(orphan, {});
        QCOMPARE(prepared.markdown, orphan);
        QVERIFY(prepared.imagePaths.isEmpty());
    }
};

QTEST_MAIN(ComposerPreviewMarkdownTest)
#include "ComposerPreviewMarkdownTest.moc"
