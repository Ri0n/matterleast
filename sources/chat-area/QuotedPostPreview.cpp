#include "QuotedPostPreview.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "QuotedReplyFormat.h"
#include "QuotedAttachmentSummary.h"
#include "backend/emoji/EmojiRegistry.h"
#include "backend/types/BackendPost.h"
#include "post/MessageFormatter.h"
#include "post/attachments/PostAttachmentList.h"

namespace Mattermost {

namespace {

// QTextBrowser still scrolls its document when both scrollbars are hidden.
// A quoted excerpt is a non-scrolling part of the surrounding chat timeline.
// Text-only interactive link: unlike a native tool button it has no chrome,
// padding or style-defined text color. Resolve QPalette::Link at paint time so
// light/dark palette changes propagate without a stylesheet.
class QuoteExpandLinkButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

    QSize sizeHint() const override
    {
        return fontMetrics().size(Qt::TextShowMnemonic, text());
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
            || event->type() == QEvent::Hide) {
            hovered = event->type() == QEvent::Enter;
            update();
        } else if (event->type() == QEvent::PaletteChange
                   || event->type() == QEvent::FocusIn
                   || event->type() == QEvent::FocusOut) {
            update();
        }
        return QToolButton::event(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setLayoutDirection(layoutDirection());
        painter.setPen(palette().color(QPalette::Link));
        painter.setOpacity(isEnabled() ? (hovered ? 1.0 : 0.8) : 0.4);
        QFont labelFont = font();
        labelFont.setUnderline(hasFocus());
        painter.setFont(labelFont);
        painter.drawText(rect(),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic,
                         text());
    }

private:
    bool hovered = false;
};

class FixedExcerptBrowser final : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        verticalScrollBar()->setValue(0);
        // Do not consume the wheel: let the enclosing chat viewport scroll.
        event->ignore();
    }
};

} // namespace

QuotedPostPreview::QuotedPostPreview(QWidget* parent, int maximumLinesValue)
    : QFrame(parent)
    , maximumLines(std::max(1, maximumLinesValue))
{
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(7);

    bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("messageQuoteBar"));
    bar->setFixedWidth(3);
    bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    bar->setBackgroundRole(QPalette::Window);
    bar->setAutoFillBackground(true);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(bar);

    contentLayout = new QVBoxLayout;
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    authorLabel = new QLabel(this);
    QFont authorFont = authorLabel->font();
    authorFont.setBold(true);
    authorLabel->setFont(authorFont);
    authorLabel->setTextFormat(Qt::PlainText);
    authorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    authorLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    messageBrowser = new FixedExcerptBrowser(this);
    messageBrowser->setObjectName(QStringLiteral("quotedPostExcerpt"));
    messageBrowser->setReadOnly(true);
    messageBrowser->setOpenLinks(false);
    messageBrowser->setOpenExternalLinks(false);
    messageBrowser->setFrameShape(QFrame::NoFrame);
    messageBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messageBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // QTextEdit::WidgetWidth rewrites the document page width when its
    // viewport is resized. During an outer chat resize this can overwrite
    // the width used to decide whether Show more is needed (Qt 5 in
    // particular may keep the former narrow document after widening).
    // Control wrapping through QTextEdit's own API instead.
    messageBrowser->setLineWrapMode(QTextEdit::FixedPixelWidth);
    messageBrowser->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    messageBrowser->setFocusPolicy(Qt::NoFocus);
    messageBrowser->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    messageBrowser->setFixedHeight(
        messageBrowser->fontMetrics().lineSpacing() * maximumLines + 2);
    messageBrowser->setContentsMargins(0, 0, 0, 0);
    messageBrowser->document()->setDocumentMargin(0);
    messageBrowser->viewport()->setAutoFillBackground(false);
    messageBrowser->viewport()->installEventFilter(this);
    connect(messageBrowser->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { scheduleTextLayout(); });
    // Selection changes, document reflow and Qt's hidden scrollbar handling
    // can move the visible excerpt even without a visible scrollbar.
    connect(messageBrowser->verticalScrollBar(), &QScrollBar::valueChanged,
            messageBrowser, [this](int value) {
                if (value != 0) {
                    messageBrowser->verticalScrollBar()->setValue(0);
                }
            });

    expandButton = new QuoteExpandLinkButton(this);
    expandButton->setObjectName(QStringLiteral("quotedPostShowMore"));
    expandButton->setAutoRaise(true);
    expandButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    expandButton->setFocusPolicy(Qt::StrongFocus);
    expandButton->setCursor(Qt::PointingHandCursor);
    expandButton->setText(tr("Show more…"));
    expandButton->hide();
    connect(expandButton, &QToolButton::clicked, this, [this] {
        expanded = !expanded;
        updateTextLayout();
    });

    attachmentSummary = new QuotedAttachmentSummary(this);

    contentLayout->addWidget(authorLabel);
    contentLayout->addWidget(messageBrowser);
    contentLayout->addWidget(expandButton, 0, Qt::AlignLeft);
    contentLayout->addWidget(attachmentSummary);
    layout->addLayout(contentLayout, 1);

    refreshPalette();
}

QuotedPostPreview::~QuotedPostPreview() = default;

void QuotedPostPreview::setPost(const BackendPost& post)
{
    setPreview(QObject::tr("Replying to %1").arg(post.getDisplayAuthorName()),
               post.message,
               !post.files.empty());

    if (attachmentSummary) {
        attachmentSummary->setFiles(post.files);
    }
}

void QuotedPostPreview::setPreview(const QString& title,
                                   const QString& message,
                                   bool hasAttachments)
{
    if (attachmentList) {
        contentLayout->removeWidget(attachmentList);
        delete attachmentList;
        attachmentList = nullptr;
    }
    _isMarkdownQuote = false;
    _emojiRegistry = nullptr;
    authorLabel->show();
    messageBrowser->setObjectName(QStringLiteral("quotedPostExcerpt"));
    messageBrowser->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    messageBrowser->setOpenExternalLinks(false);
    messageBrowser->setOpenLinks(false);

    const QString visibleMessage = QuotedReplyFormat::stripFallback(message);
    const QString newText = visibleMessage.trimmed().isEmpty()
        ? (hasAttachments ? QString() : QStringLiteral("[empty message]"))
        : visibleMessage;
    // Rerendering the same reference (for example on palette change) must
    // not collapse a quote that the user explicitly expanded.
    if (authorLabel->text() != title || fullText != newText) {
        expanded = false;
    }
    authorLabel->setText(title);
    fullText = newText;
    if (attachmentSummary) {
        attachmentSummary->setGenericAttachment(hasAttachments);
    }
    // Keep the full excerpt as a frame fallback. The browser uses per-link
    // tooltips supplied centrally by MessageFormatter.
    setToolTip(visibleMessage);
    refreshText();
}

void QuotedPostPreview::setMarkdownQuote(const QString& markdown,
                                         EmojiRegistry* registry)
{
    // Unlike a referenced post, this is user-authored Markdown inside the
    // message itself. Preserve the source verbatim, including fallback-looking
    // text, and permit selecting and opening links in the quoted body.
    if (!_isMarkdownQuote || fullText != markdown) {
        expanded = false;
    }
    _isMarkdownQuote = true;
    _emojiRegistry = registry;
    fullText = markdown;
    authorLabel->hide();
    attachmentSummary->hide();
    messageBrowser->setObjectName(QStringLiteral("messageQuoteRichText"));
    messageBrowser->setTextInteractionFlags(
        Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    messageBrowser->setOpenExternalLinks(true);
    messageBrowser->setOpenLinks(true);
    refreshText();
}

void QuotedPostPreview::invalidateTextLayout()
{
    // Image resources can arrive after the initial document clone was
    // measured. The next measurement must see the new image dimensions.
    measuringDocument.reset();
    scheduleTextLayout();
}

void QuotedPostPreview::setInteractiveAttachments(
    Backend& backend,
    const std::list<BackendFile>& files,
    const QString& authorName)
{
    if (files.empty() || !contentLayout) {
        return;
    }

    if (attachmentSummary) {
        attachmentSummary->setGenericAttachment(false);
    }

    if (attachmentList) {
        contentLayout->removeWidget(attachmentList);
        delete attachmentList;
    }

    attachmentList = new PostAttachmentList(backend, this);
    connect(attachmentList,
            &PostAttachmentList::dimensionsChanged,
            this,
            [this] {
                updateGeometry();
                emit dimensionsChanged();
            });

    for (const BackendFile& file : files) {
        attachmentList->addFile(file, authorName);
    }
    // Embedded file cards should stay clickable, but their inner list is not
    // a second scrollable timeline. Ignore wheels over its viewport so the
    // surrounding chat continues to scroll, including over download cards.
    if (auto* list = attachmentList->findChild<QListWidget*>()) {
        list->viewport()->installEventFilter(this);
    }

    // Qt::AlignLeft suppresses horizontal expansion: PostAttachmentList's
    // initial size hint comes from a very narrow, not-yet-laid-out viewport.
    // Its QText labels then wrap almost character by character, and the
    // download/open controls overlap. Let the parent preview give the list
    // its full available width; the list handles height-for-width itself.
    contentLayout->addWidget(attachmentList);
    updateGeometry();
    emit dimensionsChanged();
}

void QuotedPostPreview::setActivatedCallback(std::function<void()> callback)
{
    activatedCallback = std::move(callback);
    setCursor(activatedCallback ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (messageBrowser) {
        messageBrowser->viewport()->setCursor(
            activatedCallback ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}

void QuotedPostPreview::setLinkActivatedCallback(
    std::function<void(const QUrl&)> callback)
{
    linkActivatedCallback = std::move(callback);
}

bool QuotedPostPreview::eventFilter(QObject* watched, QEvent* event)
{
    if (messageBrowser && watched == messageBrowser->viewport() && event
        && (event->type() == QEvent::Resize
            || event->type() == QEvent::LayoutRequest)) {
        scheduleTextLayout();
    }
    if (event && event->type() == QEvent::Wheel && attachmentList) {
        if (auto* list = attachmentList->findChild<QListWidget*>()) {
            if (watched == list->viewport()) {
                event->ignore();
                return true;
            }
        }
    }
    if (messageBrowser && watched == messageBrowser->viewport() && event
        && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint position = mouseEvent->position().toPoint();
#else
            const QPoint position = mouseEvent->pos();
#endif
            const QString anchor = messageBrowser->anchorAt(position);
            if (!anchor.isEmpty() && linkActivatedCallback) {
                linkActivatedCallback(QUrl(anchor));
                mouseEvent->accept();
                return true;
            }
            if (activatedCallback) {
                activatedCallback();
                mouseEvent->accept();
                return true;
            }
        }
    }
    return QFrame::eventFilter(watched, event);
}

void QuotedPostPreview::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange
                  || event->type() == QEvent::FontChange)) {
        refreshPalette();
        refreshText();
    }
}

void QuotedPostPreview::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    scheduleTextLayout();
}

void QuotedPostPreview::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton && activatedCallback) {
        activatedCallback();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void QuotedPostPreview::refreshPalette()
{
    if (!authorLabel || !messageBrowser) {
        return;
    }

    const QColor textColor = palette().color(QPalette::Text);
    QColor mutedColor = palette().color(QPalette::PlaceholderText);

    if (!mutedColor.isValid() || mutedColor.rgba() == textColor.rgba()) {
        mutedColor = textColor;
        mutedColor.setAlphaF(mutedColor.alphaF() * 0.65);
    }

    QPalette mutedPalette = palette();
    mutedPalette.setColor(QPalette::Text, mutedColor);
    mutedPalette.setColor(QPalette::WindowText, mutedColor);
    authorLabel->setForegroundRole(QPalette::WindowText);
    authorLabel->setPalette(mutedPalette);
    messageBrowser->setPalette(mutedPalette);
    messageBrowser->viewport()->setAutoFillBackground(false);

    if (expandButton) expandButton->update();

    if (bar) {
        QColor barColor = textColor;
        barColor.setAlphaF(0.50);
        QPalette barPalette = bar->palette();
        barPalette.setColor(QPalette::Window, barColor);
        bar->setPalette(barPalette);
    }
}

void QuotedPostPreview::refreshText()
{
    // The display QTextDocument is controlled by QTextEdit. A separate clone
    // is measured at the proposed chat width, as in Psi's ChatMessageText.
    // Recreate it only for new formatted content, not on every resize.
    measuringDocument.reset();
    if (!messageBrowser || fullText.isEmpty()) {
        if (messageBrowser) {
            messageBrowser->clear();
            messageBrowser->hide();
        }
        if (expandButton) expandButton->hide();
        scheduleTextLayout();
        return;
    }

    messageBrowser->show();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    // Keep the parsed Markdown document intact. Serializing it through HTML and
    // feeding that HTML into QTextBrowser loses structural/soft line boundaries
    // (notably consecutive quote lines), which made the edit/reply preview
    // collapse user-visible newlines even though the composer source was intact.
    messageBrowser->document()->setDefaultFont(messageBrowser->font());
    MessageFormatter::buildMarkdownDocument(
        *messageBrowser->document(), fullText, _emojiRegistry,
        palette().color(QPalette::Text));
#else
    messageBrowser->setHtml(MessageFormatter::formatMessageText(fullText, _emojiRegistry));
#endif
    messageBrowser->document()->setDocumentMargin(0);
    messageBrowser->verticalScrollBar()->setValue(0);
    scheduleTextLayout();
}

void QuotedPostPreview::scheduleTextLayout()
{
    if (textLayoutScheduled) return;
    textLayoutScheduled = true;
    // QTextBrowser lays out its viewport after the parent resize. Measure
    // after that pass, not while Qt is still resizing the text document.
    QTimer::singleShot(0, this, [this] {
        textLayoutScheduled = false;
        updateTextLayout();
    });
}

void QuotedPostPreview::updateTextLayout()
{
    if (!messageBrowser || !expandButton || fullText.isEmpty()) return;

    // Derive the wrap width from the preview itself. The QTextBrowser
    // viewport may still have its previous width during the parent's
    // layout pass. FixedPixelWidth prevents QTextEdit's own resize handler
    // from replacing this width with a stale viewport measurement.
    const QMargins outerMargins = layout()->contentsMargins();
    const QMargins columnMargins = contentLayout->contentsMargins();
    const int availableWidth = std::max(1,
        width() - outerMargins.left() - outerMargins.right()
        - bar->width() - layout()->spacing()
        - columnMargins.left() - columnMargins.right()
        - messageBrowser->frameWidth() * 2);
    if (messageBrowser->lineWrapColumnOrWidth() != availableWidth) {
        messageBrowser->setLineWrapColumnOrWidth(availableWidth);
    }
    if (!measuringDocument) {
        QTextDocument* source = messageBrowser->document();
        measuringDocument.reset(source->clone());
        // QTextDocument::clone() does not preserve all document-level
        // settings. Use the same metrics/margins as the displayed excerpt.
        measuringDocument->setDocumentMargin(source->documentMargin());
        measuringDocument->setUseDesignMetrics(source->useDesignMetrics());
    }
    measuringDocument->setTextWidth(availableWidth);
    const int fullHeight = std::max(1, static_cast<int>(
        std::ceil(measuringDocument->documentLayout()->documentSize().height())) + 2);
    const int collapsedHeight =
        messageBrowser->fontMetrics().lineSpacing() * maximumLines + 2;
    // QTextDocument uses fractional font metrics; on Qt 5 two rendered lines
    // can measure 41px while QFontMetrics::lineSpacing() reports 20px each.
    // Reserve up to one rounding pixel per visible line before deciding that
    // a quote needs a Show more action. Never clip that tolerated remainder.
    const bool truncated = fullHeight > collapsedHeight + maximumLines;
    const int targetHeight = expanded || !truncated ? fullHeight : collapsedHeight;
    const bool geometryChanged =
        messageBrowser->height() != targetHeight
        || expandButton->isHidden() == truncated;

    messageBrowser->setFixedHeight(targetHeight);
    expandButton->setText(expanded ? tr("Show less") : tr("Show more…"));
    expandButton->setVisible(truncated);
    // An expanded quote is already readable in the post; a full-text tooltip
    // would obscure it while the mouse is resting over the body.
    const QString tooltip = expanded || _isMarkdownQuote ? QString() : fullText;
    setToolTip(tooltip);
    messageBrowser->verticalScrollBar()->setValue(0);

    if (geometryChanged) {
        messageBrowser->updateGeometry();
        updateGeometry();
        emit dimensionsChanged();
    }
}

} // namespace Mattermost
