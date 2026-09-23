#include "InteractiveTextEdit.h"

#include <algorithm>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QFocusEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QListView>
#include <QModelIndex>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

namespace Mattermost {
namespace {

constexpr int CompletionInsertRole = Qt::UserRole + 1;
constexpr int CompletionPopupDelayMs = 300;
constexpr int CompletionMaximumVisibleItems = 8;
constexpr int CompletionMinimumWidth = 280;

bool completionCandidateMatches(
    const InteractiveTextEdit::CompletionCandidate& candidate,
    const QString& query)
{
    if (query.isEmpty()) {
        return true;
    }

    if (candidate.displayText.contains(query, Qt::CaseInsensitive)
        || candidate.detailText.contains(query, Qt::CaseInsensitive)) {
        return true;
    }

    for (const QString& key : candidate.filterKeys) {
        if (key.contains(query, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool isModifierKey(int key)
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
        return true;
    default:
        return false;
    }
}

bool isPopupNavigationKey(int key)
{
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Enter:
    case Qt::Key_Return:
    case Qt::Key_Escape:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return true;
    default:
        return false;
    }
}

} // namespace

InteractiveTextEdit::InteractiveTextEdit(QWidget* parent)
    : QTextEdit(parent)
    , completionView(new QListView(this))
    , completionModel(new QStandardItemModel(this))
{
    completionView->setObjectName(QStringLiteral("completionOverlay"));
    completionView->setModel(completionModel);
    completionView->setFocusPolicy(Qt::NoFocus);
    completionView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    completionView->setSelectionMode(QAbstractItemView::SingleSelection);
    completionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    completionView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    completionView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    completionView->setUniformItemSizes(true);
    completionView->setFrameShape(QFrame::StyledPanel);
    completionView->hide();

    connect(completionView, &QListView::clicked,
            this, [this](const QModelIndex& index) {
        acceptCompletion(index);
    });

    completionDelayTimer.setSingleShot(true);
    completionDelayTimer.setInterval(CompletionPopupDelayMs);
    connect(&completionDelayTimer, &QTimer::timeout, this, [this] {
        completionMayOpen = true;
        refreshCompletion();
    });

    // Any edit immediately withdraws a visible popup. It may only reappear
    // after the corresponding key release and a short quiet period.
    connect(this, &QTextEdit::textChanged, this, [this] {
        completionDelayTimer.stop();
        completionMayOpen = false;
        hideCompletion();
    });
}

InteractiveTextEdit::~InteractiveTextEdit()
{
    // The overlay may be reparented to the editor's top-level host so it can
    // extend beyond the QTextEdit rect. Keep ownership explicitly with the
    // editor despite that visual parent.
    delete completionView;
    completionView = nullptr;
}

void InteractiveTextEdit::setCompletionRules(QVector<CompletionRule> rules)
{
    endCompletionQuery();
    completionRules.clear();
    completionRules.reserve(rules.size());
    for (CompletionRule& rule : rules) {
        if (!rule.prefix.isEmpty() && rule.provider) {
            completionRules.push_back(std::move(rule));
        }
    }
    completionDelayTimer.stop();
    completionMayOpen = false;
    hideCompletion();
}

void InteractiveTextEdit::addCompletionRule(CompletionRule rule)
{
    if (rule.prefix.isEmpty() || !rule.provider) {
        return;
    }
    completionRules.push_back(std::move(rule));
}

void InteractiveTextEdit::clearCompletionRules()
{
    endCompletionQuery();
    completionRules.clear();
    completionDelayTimer.stop();
    completionMayOpen = false;
    hideCompletion();
}

bool InteractiveTextEdit::completionPopupVisible() const
{
    return completionView && completionView->isVisible();
}

void InteractiveTextEdit::refreshCompletions()
{
    // Async providers may update the already-visible popup immediately. If it
    // is still hidden, never let a network/group callback bypass the user's
    // 300 ms key-release quiet period.
    if (completionPopupVisible() || completionMayOpen) {
        refreshCompletion();
    }
}

InteractiveTextEdit::ActiveCompletion InteractiveTextEdit::activeCompletion() const
{
    ActiveCompletion result;
    if (completionRules.isEmpty() || isReadOnly()) {
        return result;
    }

    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return result;
    }

    const QTextBlock block = cursor.block();
    if (!block.isValid()) {
        return result;
    }

    const int blockOffset = cursor.position() - block.position();
    if (blockOffset < 0) {
        return result;
    }

    const QString blockText = block.text();
    const QString beforeCursor = blockText.left(blockOffset);
    int tokenStart = beforeCursor.size();
    while (tokenStart > 0 && !beforeCursor.at(tokenStart - 1).isSpace()) {
        --tokenStart;
    }

    const QString typedToken = beforeCursor.mid(tokenStart);
    const int exclusionOffset = typedToken.startsWith(QLatin1Char('-')) ? 1 : 0;
    const QString ruleText = typedToken.mid(exclusionOffset);

    for (int index = 0; index < completionRules.size(); ++index) {
        const CompletionRule& rule = completionRules.at(index);
        if (!ruleText.startsWith(rule.prefix, Qt::CaseInsensitive)) {
            continue;
        }

        const int queryOffset = exclusionOffset + rule.prefix.size();
        int tokenEnd = blockOffset;
        while (tokenEnd < blockText.size() && !blockText.at(tokenEnd).isSpace()) {
            ++tokenEnd;
        }

        result.ruleIndex = index;
        result.queryStart = block.position() + tokenStart + queryOffset;
        // Replace the complete value belonging to the active prefix, not just
        // the substring before the cursor. This keeps editing an existing token
        // deterministic: `in:geneXral` with the caret at X becomes exactly the
        // selected canonical channel value.
        result.queryEnd = block.position() + tokenEnd;
        result.query = typedToken.mid(queryOffset);
        return result;
    }

    return result;
}

void InteractiveTextEdit::refreshCompletion()
{
    if (!hasFocus()) {
        completionMayOpen = false;
        endCompletionQuery();
        hideCompletion();
        return;
    }

    const ActiveCompletion active = activeCompletion();
    if (!active.isValid()) {
        completionMayOpen = false;
        endCompletionQuery();
        hideCompletion();
        return;
    }

    updateCompletionQuery(active.ruleIndex, active.query);

    activeRuleIndex = active.ruleIndex;
    activeQueryStart = active.queryStart;
    activeQueryEnd = active.queryEnd;

    rebuildCompletionModel(completionRules.at(active.ruleIndex), active.query);
    if (completionModel->rowCount() == 0) {
        hideCompletion();
        return;
    }

    ensureCompletionViewHost();
    completionView->setCurrentIndex(completionModel->index(0, 0));
    showCompletionView();
}

void InteractiveTextEdit::rebuildCompletionModel(
    const CompletionRule& rule,
    const QString& query)
{
    completionModel->clear();
    const QVector<CompletionCandidate> candidates =
        rule.provider ? rule.provider() : QVector<CompletionCandidate> {};

    for (const CompletionCandidate& candidate : candidates) {
        const QString insertText = candidate.insertText.isEmpty()
            ? candidate.displayText : candidate.insertText;
        if (candidate.displayText.isEmpty() || insertText.isEmpty()
            || !completionCandidateMatches(candidate, query)) {
            continue;
        }

        QString display = candidate.displayText;
        if (!candidate.detailText.isEmpty()
            && candidate.detailText != candidate.displayText) {
            display += QStringLiteral("  —  ") + candidate.detailText;
        }

        auto* item = new QStandardItem(display);
        item->setData(insertText, CompletionInsertRole);
        completionModel->appendRow(item);
    }
}

void InteractiveTextEdit::acceptCompletion(const QModelIndex& index)
{
    if (!index.isValid() || activeRuleIndex < 0
        || activeRuleIndex >= completionRules.size()
        || activeQueryStart < 0 || activeQueryEnd < activeQueryStart) {
        hideCompletion();
        return;
    }

    const QString insertText = index.data(CompletionInsertRole).toString();
    if (insertText.isEmpty()) {
        hideCompletion();
        return;
    }

    // QTextCursor::insertText() emits QTextEdit::textChanged synchronously.
    // Our textChanged handler hides completion, which resets activeRuleIndex and
    // the active query range. Snapshot every piece of completion state before
    // touching the document so accepting a candidate cannot re-enter here with
    // invalidated state (or index completionRules[-1]).
    const int queryStart = activeQueryStart;
    const int queryEnd = activeQueryEnd;
    const bool appendSpace = completionRules.at(activeRuleIndex).appendSpace;

    completionDelayTimer.stop();
    completionMayOpen = false;
    endCompletionQuery();
    hideCompletion();

    QTextCursor cursor = textCursor();
    cursor.setPosition(queryStart);
    cursor.setPosition(queryEnd, QTextCursor::KeepAnchor);
    cursor.insertText(insertText);
    if (appendSpace) {
        cursor.insertText(QStringLiteral(" "));
    }
    setTextCursor(cursor);
}

void InteractiveTextEdit::hideCompletion()
{
    activeRuleIndex = -1;
    activeQueryStart = -1;
    activeQueryEnd = -1;
    if (completionView) {
        completionView->hide();
    }
}

void InteractiveTextEdit::updateCompletionQueryFromCursor()
{
    if (!hasFocus()) {
        completionMayOpen = false;
        endCompletionQuery();
        hideCompletion();
        return;
    }

    const ActiveCompletion active = activeCompletion();
    if (!active.isValid()) {
        completionMayOpen = false;
        endCompletionQuery();
        hideCompletion();
        return;
    }

    updateCompletionQuery(active.ruleIndex, active.query);
}

void InteractiveTextEdit::scheduleCompletionPopup()
{
    updateCompletionQueryFromCursor();
    const ActiveCompletion active = activeCompletion();
    if (!active.isValid()) {
        completionDelayTimer.stop();
        completionMayOpen = false;
        return;
    }

    completionMayOpen = false;
    completionDelayTimer.start();
}

void InteractiveTextEdit::ensureCompletionViewHost()
{
    if (!completionView) {
        return;
    }

    QWidget* host = window();
    if (!host) {
        host = this;
    }

    if (completionView->parentWidget() != host) {
        completionView->hide();
        completionView->setParent(host);
        completionView->setWindowFlags(Qt::Widget);
        completionView->setFocusPolicy(Qt::NoFocus);
    }
}

void InteractiveTextEdit::showCompletionView()
{
    if (!completionView || completionModel->rowCount() <= 0) {
        return;
    }

    QWidget* host = completionView->parentWidget();
    if (!host) {
        return;
    }

    const QRect caret = cursorRect();
    const QPoint caretTop = viewport()->mapTo(host, caret.topLeft());
    const QPoint caretBottom = viewport()->mapTo(host, caret.bottomLeft());

    int rowHeight = completionView->sizeHintForRow(0);
    if (rowHeight <= 0) {
        rowHeight = fontMetrics().lineSpacing() + 8;
    }

    const int visibleRows = std::min(
        completionModel->rowCount(), CompletionMaximumVisibleItems);
    int height = rowHeight * visibleRows + 2 * completionView->frameWidth();
    const QRect bounds = host->contentsRect();
    height = std::min(height, std::max(rowHeight, bounds.height()));

    int width = std::max(CompletionMinimumWidth, viewport()->width());
    width = std::min(width, bounds.width());

    int x = caretTop.x();
    if (x + width > bounds.right() + 1) {
        x = bounds.right() + 1 - width;
    }
    x = std::max(bounds.left(), x);

    const int belowY = caretBottom.y() + 1;
    const int aboveY = caretTop.y() - height;
    int y = belowY;
    if (belowY + height > bounds.bottom() + 1) {
        y = aboveY;
    }
    y = std::max(bounds.top(),
                 std::min(y, bounds.bottom() + 1 - height));

    completionView->setGeometry(x, y, width, height);
    completionView->raise();
    completionView->show();

    // The overlay never participates in focus/key routing.
    setFocus(Qt::OtherFocusReason);
}

void InteractiveTextEdit::moveCompletionSelection(int delta)
{
    if (!completionPopupVisible() || !completionView || !completionModel) {
        return;
    }

    const int rows = completionModel->rowCount();
    if (rows <= 0) {
        return;
    }

    int row = completionView->currentIndex().row();
    if (row < 0) {
        row = 0;
    }
    row = std::max(0, std::min(rows - 1, row + delta));
    const QModelIndex index = completionModel->index(row, 0);
    completionView->setCurrentIndex(index);
    completionView->scrollTo(index);
}

void InteractiveTextEdit::updateCompletionQuery(int ruleIndex, const QString& query)
{
    if (queryRuleIndex == ruleIndex && queryText == query) {
        return;
    }

    if (queryRuleIndex >= 0 && queryRuleIndex < completionRules.size()
        && queryRuleIndex != ruleIndex) {
        const CompletionQueryHandler& previous = completionRules.at(queryRuleIndex).queryChanged;
        if (previous) {
            previous(QString());
        }
    }

    queryRuleIndex = ruleIndex;
    queryText = query;
    if (ruleIndex >= 0 && ruleIndex < completionRules.size()) {
        const CompletionQueryHandler& handler = completionRules.at(ruleIndex).queryChanged;
        if (handler) {
            handler(query);
        }
    }
}

void InteractiveTextEdit::endCompletionQuery()
{
    if (queryRuleIndex >= 0 && queryRuleIndex < completionRules.size()) {
        const CompletionQueryHandler& handler = completionRules.at(queryRuleIndex).queryChanged;
        if (handler) {
            handler(QString());
        }
    }
    queryRuleIndex = -1;
    queryText.clear();
}

void InteractiveTextEdit::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    completionDelayTimer.stop();
    completionMayOpen = false;

    if (completionPopupVisible()) {
        switch (event->key()) {
        case Qt::Key_Up:
            moveCompletionSelection(-1);
            event->accept();
            return;
        case Qt::Key_Down:
            moveCompletionSelection(1);
            event->accept();
            return;
        case Qt::Key_PageUp:
            moveCompletionSelection(
                -std::max(1, CompletionMaximumVisibleItems - 1));
            event->accept();
            return;
        case Qt::Key_PageDown:
            moveCompletionSelection(
                std::max(1, CompletionMaximumVisibleItems - 1));
            event->accept();
            return;
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            if (completionView) {
                const QModelIndex current = completionView->currentIndex();
                if (current.isValid()) {
                    acceptCompletion(current);
                }
            }
            event->accept();
            return;
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            hideCompletion();
            event->accept();
            return;
        default:
            // The suggestion list is an ordinary child overlay in this same
            // top-level window, so it never owns keyboard input. Editing keys
            // are ordinary QTextEdit input; withdraw suggestions first.
            hideCompletion();
            break;
        }
    }

    const bool returnKey = event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return;
    const bool shiftPressed = event->modifiers() & Qt::ShiftModifier;
    const bool ctrlPressed = event->modifiers() & Qt::ControlModifier;

    if (submitOnEnter && returnKey && !shiftPressed) {
        const bool shouldSubmit = submitOnCtrlEnter ? ctrlPressed : !ctrlPressed;
        if (shouldSubmit) {
            if (submitHandler) {
                submitHandler();
            }
            event->accept();
            return;
        }

        // The alternate Return shortcut is always a newline. Do this explicitly
        // instead of relying on platform-specific QTextEdit handling of
        // Ctrl+Return so the two submit modes behave identically on Qt 5/6.
        QTextCursor cursor = textCursor();
        cursor.insertBlock();
        setTextCursor(cursor);
        event->accept();
        return;
    }

    QTextEdit::keyPressEvent(event);
}

void InteractiveTextEdit::keyReleaseEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    QTextEdit::keyReleaseEvent(event);

    if (isModifierKey(event->key())) {
        return;
    }

    if (completionPopupVisible() && isPopupNavigationKey(event->key())) {
        return;
    }

    scheduleCompletionPopup();
}

void InteractiveTextEdit::focusOutEvent(QFocusEvent* event)
{
    completionDelayTimer.stop();
    completionMayOpen = false;
    endCompletionQuery();
    hideCompletion();
    QTextEdit::focusOutEvent(event);
}

} // namespace Mattermost
