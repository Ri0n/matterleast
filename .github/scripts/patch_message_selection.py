from pathlib import Path


def read(path):
    return Path(path).read_text()


def write(path, text):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing block in {path}: {old[:160]!r}")
    write(path, text.replace(old, new, 1))


def insert_before(path, marker, block):
    text = read(path)
    pos = text.find(marker)
    if pos < 0:
        raise SystemExit(f"missing marker in {path}: {marker!r}")
    write(path, text[:pos] + block + text[pos:])


# --- Sidebar: create a custom group and immediately move the selected channel. ---
replace_once(
    "sources/backend/SidebarService.h",
    "    void updateCategory(const SidebarCategory& category,\n",
    "    void createCategory(const QString& teamId, const QString& displayName,\n"
    "                        std::function<void(const SidebarCategory&)> callback = {});\n"
    "    void updateCategory(const SidebarCategory& category,\n")

insert_before(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::updateCategory(",
    r'''void SidebarService::createCategory(
    const QString& teamId,
    const QString& displayName,
    std::function<void(const SidebarCategory&)> callback)
{
    const QString name = displayName.trimmed();
    if (teamId.isEmpty() || name.isEmpty() || currentUserId().isEmpty()) {
        return;
    }

    SidebarCategory category;
    category.userId = currentUserId();
    category.teamId = teamId;
    category.sorting = QStringLiteral("manual");
    category.type = QStringLiteral("custom");
    category.displayName = name;

    QJsonObject payload = category.toJson();
    payload.remove(QStringLiteral("id"));
    payload.remove(QStringLiteral("sort_order"));

    NetworkRequest request(categoriesPath(teamId));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
        SidebarCategory created = SidebarCategory::fromJson(doc.object());
        if (created.id.isEmpty()) {
            return;
        }
        SidebarTeamState& state = sidebarByTeam[teamId];
        state.categories.insert(created.id, created);
        if (!state.order.contains(created.id)) {
            state.order.push_back(created.id);
        }
        emit categoriesChanged(teamId);
        if (callback) {
            callback(state.categories[created.id]);
        }
    }));
}

''')

replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid moveChannelToCategory(ChannelItem* item, const QString& categoryId);\n",
    "\tvoid moveChannelToCategory(ChannelItem* item, const QString& categoryId);\n"
    "\tvoid createGroupAndMoveChannel(ChannelItem* item);\n")

replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "#include <QHeaderView>\n",
    "#include <QHeaderView>\n#include <QInputDialog>\n")

insert_before(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::moveChannelToCategory(ChannelItem* item, const QString& categoryId)",
    r'''void ChannelTree::createGroupAndMoveChannel(ChannelItem* item)
{
    if (!backendForSidebar || !item || !item->parent()) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString channelId = item->data(0, ItemIdRole).toString();
    const QString sourceCategoryId = item->parent()->data(0, ItemIdRole).toString();
    if (teamId.isEmpty() || channelId.isEmpty() || sourceCategoryId.isEmpty()) {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Create group"), tr("Group name:"), QLineEdit::Normal,
        QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    QPointer<ChannelTree> guard(this);
    SidebarService::instance(*backendForSidebar).createCategory(
        teamId, name,
        [guard, teamId, sourceCategoryId, channelId](const SidebarCategory& created) {
            if (!guard || !guard->backendForSidebar || created.id.isEmpty()) {
                return;
            }

            auto& sidebar = SidebarService::instance(*guard->backendForSidebar);
            SidebarTeamState* state = sidebar.teamState(teamId);
            const SidebarCategory* source = state ? state->category(sourceCategoryId) : nullptr;
            const SidebarCategory* target = state ? state->category(created.id) : nullptr;
            if (!target) {
                return;
            }

            QVector<SidebarCategory> updates;
            if (source && source->id != target->id) {
                SidebarCategory sourceUpdate = *source;
                sourceUpdate.channelIds.removeAll(channelId);
                updates.push_back(std::move(sourceUpdate));
            }

            SidebarCategory targetUpdate = *target;
            targetUpdate.channelIds.removeAll(channelId);
            targetUpdate.channelIds.push_back(channelId);
            targetUpdate.sorting = QStringLiteral("manual");
            updates.push_back(std::move(targetUpdate));

            sidebar.updateCategories(teamId, updates,
                [guard, teamId](const SidebarTeamState&) {
                    if (guard) {
                        guard->refreshSidebarTeam(teamId);
                    }
                });
        });
}

''')

replace_once(
    "sources/channel-tree/ChannelItem.cpp",
    '''        const auto targets = tree->customCategoryTargets(this);
        if (!targets.isEmpty()) {
            QMenu* moveMenu = menu.addMenu(QStringLiteral("Move to group"));
            for (const auto& target : targets) {
                moveMenu->addAction(target.second, [tree, this, categoryId = target.first] {
                    tree->moveChannelToCategory(this, categoryId);
                });
            }
        }
''',
    '''        const auto targets = tree->customCategoryTargets(this);
        QMenu* moveMenu = menu.addMenu(QStringLiteral("Move to group"));
        for (const auto& target : targets) {
            moveMenu->addAction(target.second, [tree, this, categoryId = target.first] {
                tree->moveChannelToCategory(this, categoryId);
            });
        }
        if (!targets.isEmpty()) {
            moveMenu->addSeparator();
        }
        moveMenu->addAction(QStringLiteral("Create new group…"), [tree, this] {
            tree->createGroupAndMoveChannel(this);
        });
''')

# --- Text selection clearing when whole-message selection takes ownership. ---
replace_once(
    "sources/chat-area/post/MessageContentWidget.h",
    "    QString selectedText() const;\n",
    "    QString selectedText() const;\n    void clearSelection();\n")

insert_before(
    "sources/chat-area/post/MessageContentWidget.cpp",
    "QString MessageContentWidget::selectedText() const",
    r'''void MessageContentWidget::clearSelection()
{
    const auto textEdits = findChildren<QTextEdit*>();
    for (QTextEdit* edit : textEdits) {
        if (!edit) {
            continue;
        }
        QTextCursor cursor = edit->textCursor();
        cursor.setPosition(cursor.position());
        edit->setTextCursor(cursor);
    }
    const auto plainEdits = findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* edit : plainEdits) {
        if (!edit) {
            continue;
        }
        QTextCursor cursor = edit->textCursor();
        cursor.setPosition(cursor.position());
        edit->setTextCursor(cursor);
    }
}

''')

# --- PostWidget: checkbox gutter, subtle selection paint, hover reaction affordance, cleaner menu. ---
replace_once(
    "sources/chat-area/post/PostWidget.h",
    "class QContextMenuEvent;\nclass QEvent;\n",
    "class QCheckBox;\nclass QContextMenuEvent;\nclass QEvent;\nclass QGraphicsOpacityEffect;\nclass QPaintEvent;\nclass QPropertyAnimation;\nclass QResizeEvent;\n")

replace_once(
    "sources/chat-area/post/PostWidget.h",
    "    QString formatForClipboardSelection (FormatType formatType) const;\n\n    void clearMessageText ();\n",
    "    QString formatForClipboardSelection (FormatType formatType) const;\n"
    "    void clearTextSelection();\n"
    "    void setWholeMessageSelectionMode(bool enabled);\n"
    "    void setWholeMessageSelected(bool selected);\n"
    "    bool wholeMessageSelectionMode() const { return wholeMessageSelectionMode_; }\n"
    "    bool wholeMessageSelected() const { return wholeMessageSelected_; }\n\n"
    "    void clearMessageText ();\n")

replace_once(
    "sources/chat-area/post/PostWidget.h",
    "signals:\n\tvoid dimensionsChanged ();\n",
    "signals:\n\tvoid dimensionsChanged ();\n"
    "    void wholeMessageSelectionToggled(const QString& postId, bool selected);\n")

replace_once(
    "sources/chat-area/post/PostWidget.h",
    "    void changeEvent(QEvent* event) override;\n    void contextMenuEvent(QContextMenuEvent* event) override;\n",
    "    bool event(QEvent* event) override;\n"
    "    void changeEvent(QEvent* event) override;\n"
    "    void contextMenuEvent(QContextMenuEvent* event) override;\n"
    "    void paintEvent(QPaintEvent* event) override;\n"
    "    void resizeEvent(QResizeEvent* event) override;\n")

replace_once(
    "sources/chat-area/post/PostWidget.h",
    "    void showPostContextMenu(const QPoint& globalPos);\n",
    "    void showPostContextMenu(const QPoint& globalPos);\n"
    "    void animateReactionAffordance(bool visible);\n"
    "    void positionReactionAffordance();\n")

replace_once(
    "sources/chat-area/post/PostWidget.h",
    "    ThreadSummaryWidget*                threadSummary = nullptr;\n",
    "    ThreadSummaryWidget*                threadSummary = nullptr;\n"
    "    QCheckBox*                         wholeMessageCheck_ = nullptr;\n"
    "    QPushButton*                       reactionAffordance_ = nullptr;\n"
    "    QGraphicsOpacityEffect*            reactionOpacity_ = nullptr;\n"
    "    QPropertyAnimation*                reactionAnimation_ = nullptr;\n"
    "    bool                               reactionAffordanceWanted_ = false;\n"
    "    bool                               wholeMessageSelectionMode_ = false;\n"
    "    bool                               wholeMessageSelected_ = false;\n")

# Includes for PostWidget.cpp.
text = read("sources/chat-area/post/PostWidget.cpp")
for inc in [
    "#include <QCheckBox>\n",
    "#include <QGraphicsOpacityEffect>\n",
    "#include <QPainter>\n",
    "#include <QPropertyAnimation>\n",
    "#include <QSignalBlocker>\n",
    "#include <QTimer>\n",
]:
    if inc not in text:
        text = text.replace("#include <QContextMenuEvent>\n", "#include <QContextMenuEvent>\n" + inc, 1)
for inc in ["#include \"ui/EmojiFont.h\"\n", "#include \"ui/IconUtils.h\"\n"]:
    if inc not in text:
        text = text.replace("#include \"ui/AvatarUtils.h\"\n", "#include \"ui/AvatarUtils.h\"\n" + inc, 1)
write("sources/chat-area/post/PostWidget.cpp", text)

replace_once(
    "sources/chat-area/post/PostWidget.cpp",
    "\tui->setupUi(this);\n",
    r'''	ui->setupUi(this);

    wholeMessageCheck_ = new QCheckBox(this);
    wholeMessageCheck_->setToolTip(tr("Select message"));
    wholeMessageCheck_->setAccessibleName(tr("Select message"));
    wholeMessageCheck_->setVisible(false);
    ui->horizontalLayout_2->insertWidget(0, wholeMessageCheck_, 0, Qt::AlignTop);
    connect(wholeMessageCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        wholeMessageSelected_ = checked;
        update();
        emit wholeMessageSelectionToggled(post.id, checked);
    });

    reactionAffordance_ = new QPushButton(QString::fromUtf8("❤️"), this);
    reactionAffordance_->setFlat(true);
    reactionAffordance_->setFixedSize(28, 28);
    reactionAffordance_->setCursor(Qt::PointingHandCursor);
    reactionAffordance_->setToolTip(tr("Add reaction"));
    reactionAffordance_->setAccessibleName(tr("Add reaction"));
    QFont reactionFont = EmojiFont::applySystemEmojiFamily(reactionAffordance_->font());
    reactionFont.setPointSize(14);
    reactionAffordance_->setFont(reactionFont);
    reactionOpacity_ = new QGraphicsOpacityEffect(reactionAffordance_);
    reactionOpacity_->setOpacity(0.0);
    reactionAffordance_->setGraphicsEffect(reactionOpacity_);
    reactionAnimation_ = new QPropertyAnimation(reactionOpacity_, "opacity", this);
    reactionAnimation_->setDuration(140);
    reactionAffordance_->hide();
    connect(reactionAnimation_, &QPropertyAnimation::finished, this, [this] {
        if (reactionAffordance_ && !reactionAffordanceWanted_) {
            reactionAffordance_->hide();
        }
    });
    connect(reactionAffordance_, &QPushButton::clicked, this, [this] {
        showEmojiDialog([this](Emoji emoji) {
            backend_.addPostReaction(post.id, emoji.name);
        });
    });
''')

# Replace the context menu as one semantic unit.
path = "sources/chat-area/post/PostWidget.cpp"
text = read(path)
start = text.find("void PostWidget::showPostContextMenu(const QPoint& globalPos)")
end = text.find("void PostWidget::setAuthor(", start)
if start < 0 or end < 0:
    raise SystemExit("cannot locate PostWidget context menu")
menu_impl = r'''void PostWidget::showPostContextMenu(const QPoint& globalPos)
{
    if (post.isDeleted) {
        return;
    }

    QMenu menu(this);
    const auto icon = [](const QString& path) { return IconUtils::symbolicIcon(path); };

    if (parentChatArea) {
        QAction* replyAction = menu.addAction(icon(QStringLiteral(":/icons/message-balloon")),
                                             tr("Reply"));
        connect(replyAction, &QAction::triggered, this, [this] {
            if (parentChatArea) {
                QuotedReplyController::instance(*parentChatArea).begin(post);
            }
        });
        menu.addSeparator();
    }

    if (post.isOwnPost()) {
        if (parentChatArea) {
            QAction* editAction = menu.addAction(icon(QStringLiteral(":/icons/edit")), tr("Edit"));
            connect(editAction, &QAction::triggered, this, [this] {
                parentChatArea->editPost(post);
            });
        }
        QAction* deleteAction = menu.addAction(icon(QStringLiteral(":/icons/trash")), tr("Delete"));
        connect(deleteAction, &QAction::triggered, this, [this] {
            backend_.deletePost(post.id);
        });
        menu.addSeparator();
    }

    if (!hoveredLink.isEmpty()) {
        QAction* copyLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                tr("Copy link to clipboard"));
        connect(copyLinkAction, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(hoveredLink);
        });
    }

    QAction* copyMessageLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                    tr("Copy message link"));
    connect(copyMessageLinkAction, &QAction::triggered, this, [this] {
        const QString link = messagePermalink(post);
        if (!link.isEmpty()) {
            QApplication::clipboard()->setText(link);
        }
    });

    const QString selectedText = getSelectedText();
    if (!selectedText.isEmpty()) {
        QAction* copySelectedAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                     tr("Copy selected text"));
        connect(copySelectedAction, &QAction::triggered, this, [selectedText] {
            QApplication::clipboard()->setText(selectedText);
        });
    }

    QAction* copyMessageAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                tr("Copy post message"));
    connect(copyMessageAction, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(formatForClipboardSelection(messageOnly));
    });

    QAction* saveAction = menu.addAction(icon(QStringLiteral(":/icons/bookmark")),
                                         tr("Save message"));
    connect(saveAction, &QAction::triggered, this, [this] {
        backend_.updateUserPreferences(BackendUserPreferences {
            QStringLiteral("flagged_post"), post.id, QStringLiteral("true")});
    });

    if (post.author) {
        menu.addSeparator();
        QAction* profileAction = menu.addAction(
            icon(QStringLiteral(":/icons/members")),
            tr("View %1's profile").arg(post.author->getDisplayName()));
        connect(profileAction, &QAction::triggered, this, [this] {
            if (!post.author) {
                return;
            }
            auto* dialog = new UserProfileDialog(backend_, *post.author, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });
    }

    menu.exec(globalPos);
}

'''
write(path, text[:start] + menu_impl + text[end:])

# Insert whole-message and hover-reaction helpers before the menu implementation.
insert_before(
    "sources/chat-area/post/PostWidget.cpp",
    "void PostWidget::showPostContextMenu(const QPoint& globalPos)",
    r'''bool PostWidget::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (!event) {
        return handled;
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
        animateReactionAffordance(true);
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
        QPointer<PostWidget> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (!guard) {
                return;
            }
            const QPoint local = guard->mapFromGlobal(QCursor::pos());
            guard->animateReactionAffordance(guard->rect().contains(local));
        });
    }
    return handled;
}

void PostWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!wholeMessageSelected_) {
        return;
    }
    QColor selected = palette().color(QPalette::Highlight);
    selected.setAlpha(34);
    QPainter painter(this);
    painter.fillRect(rect(), selected);
}

void PostWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionReactionAffordance();
}

void PostWidget::setWholeMessageSelectionMode(bool enabled)
{
    if (wholeMessageSelectionMode_ == enabled) {
        return;
    }
    wholeMessageSelectionMode_ = enabled;
    if (wholeMessageCheck_) {
        wholeMessageCheck_->setVisible(enabled);
    }
    if (enabled) {
        clearTextSelection();
        animateReactionAffordance(false);
    }
    updateGeometry();
    update();
}

void PostWidget::setWholeMessageSelected(bool selected)
{
    wholeMessageSelected_ = selected;
    if (wholeMessageCheck_) {
        const QSignalBlocker blocker(wholeMessageCheck_);
        wholeMessageCheck_->setChecked(selected);
    }
    update();
}

void PostWidget::clearTextSelection()
{
    if (messageContent) {
        messageContent->clearSelection();
    }
    if (ui && ui->authorName && ui->authorName->selectionStart() >= 0) {
        ui->authorName->setSelection(0, 0);
    }
}

void PostWidget::animateReactionAffordance(bool visible)
{
    visible = visible && !wholeMessageSelectionMode_ && !post.isDeleted;
    reactionAffordanceWanted_ = visible;
    if (!reactionAffordance_ || !reactionOpacity_ || !reactionAnimation_) {
        return;
    }
    reactionAnimation_->stop();
    if (visible) {
        positionReactionAffordance();
        reactionAffordance_->show();
        reactionAffordance_->raise();
    }
    reactionAnimation_->setStartValue(reactionOpacity_->opacity());
    reactionAnimation_->setEndValue(visible ? 1.0 : 0.0);
    reactionAnimation_->start();
}

void PostWidget::positionReactionAffordance()
{
    if (!reactionAffordance_) {
        return;
    }
    const int x = 4;
    const int y = std::max(4, height() - reactionAffordance_->height() - 6);
    reactionAffordance_->move(x, y);
}

''')

# --- Message-selection coordinator lives in ChatLogWidget; it only remeasures existing widgets. ---
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "#include <QPointer>\n#include <QString>\n",
    "#include <QHash>\n#include <QPointer>\n#include <QSet>\n#include <QString>\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "class PostWidget;\n",
    "class PostWidget;\nclass QFrame;\nclass QLabel;\nclass QPushButton;\nclass QResizeEvent;\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    void postEditFinished();\n",
    "    void postEditFinished();\n\n"
    "    bool isMessageSelectionMode() const { return messageSelectionMode_; }\n"
    "    void beginMessageSelectionDrag(const QString& anchorPostId, const QString& currentPostId);\n"
    "    void updateMessageSelectionDrag(const QString& currentPostId);\n"
    "    void finishMessageSelectionDrag();\n"
    "    void cancelMessageSelection();\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    void destroyItemWidget(int index, QWidget* widget) override;\n",
    "    void destroyItemWidget(int index, QWidget* widget) override;\n"
    "    void resizeEvent(QResizeEvent* event) override;\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    void updateReadCursorFromViewport();\n",
    "    void updateReadCursorFromViewport();\n"
    "    void setMessageSelectionRange(const QString& currentPostId);\n"
    "    void setMessagePostSelected(const QString& postId, bool selected);\n"
    "    void applyMessageSelectionVisuals();\n"
    "    void cacheSelectedPost(const QString& postId);\n"
    "    void ensureSelectionToolbar();\n"
    "    void updateSelectionToolbar();\n"
    "    void positionSelectionToolbar();\n"
    "    void copySelectedPosts();\n"
    "    void deleteSelectedOwnPosts();\n")
replace_once(
    "sources/chat-area/ChatLogWidget.h",
    "    bool _initialScrollBarPulsePending = true;\n",
    "    bool _initialScrollBarPulsePending = true;\n\n"
    "    bool messageSelectionMode_ = false;\n"
    "    bool messageSelectionDragActive_ = false;\n"
    "    QString messageSelectionAnchorPostId_;\n"
    "    QSet<QString> selectedPostIds_;\n"
    "    QSet<QString> selectedOwnPostIds_;\n"
    "    QHash<QString, QString> selectedFormattedPosts_;\n"
    "    QFrame* selectionToolbar_ = nullptr;\n"
    "    QLabel* selectionCountLabel_ = nullptr;\n"
    "    QPushButton* selectionDeleteButton_ = nullptr;\n"
    "    QPushButton* selectionCopyButton_ = nullptr;\n")

text = read("sources/chat-area/ChatLogWidget.cpp")
for inc in [
    "#include <QApplication>\n", "#include <QClipboard>\n", "#include <QFrame>\n",
    "#include <QHBoxLayout>\n", "#include <QLabel>\n", "#include <QPushButton>\n",
    "#include <QResizeEvent>\n",
]:
    if inc not in text:
        text = text.replace("#include <QLoggingCategory>\n", "#include <QLoggingCategory>\n" + inc, 1)
if '#include "post/PostSelectionPolicy.h"\n' not in text:
    text = text.replace('#include "post/PostWidget.h"\n', '#include "post/PostWidget.h"\n#include "post/PostSelectionPolicy.h"\n', 1)
if '#include "ui/IconUtils.h"\n' not in text:
    text = text.replace('#include "ui/OverlayScrollBarManager.h"\n', '#include "ui/IconUtils.h"\n#include "ui/OverlayScrollBarManager.h"\n', 1)
write("sources/chat-area/ChatLogWidget.cpp", text)

replace_once(
    "sources/chat-area/ChatLogWidget.cpp",
    "    return widget;\n}\n\nQString ChatLogWidget::itemIdentity",
    r'''    widget->setWholeMessageSelectionMode(messageSelectionMode_);
    widget->setWholeMessageSelected(selectedPostIds_.contains(postId));
    connect(widget, &PostWidget::wholeMessageSelectionToggled,
            this, [this](const QString& id, bool selected) {
        setMessagePostSelected(id, selected);
    });
    if (selectedPostIds_.contains(postId)) {
        cacheSelectedPost(postId);
    }
    return widget;
}

QString ChatLogWidget::itemIdentity''')

# Append selection implementation before namespace close.
path = "sources/chat-area/ChatLogWidget.cpp"
text = read(path)
marker = "\n} // namespace Mattermost\n"
pos = text.rfind(marker)
if pos < 0:
    raise SystemExit("ChatLogWidget namespace end missing")
selection_impl = r'''
void ChatLogWidget::resizeEvent(QResizeEvent* event)
{
    PostListWidget::resizeEvent(event);
    positionSelectionToolbar();
}

void ChatLogWidget::beginMessageSelectionDrag(const QString& anchorPostId,
                                              const QString& currentPostId)
{
    if (!postSource || anchorPostId.isEmpty() || currentPostId.isEmpty()) {
        return;
    }
    messageSelectionMode_ = true;
    messageSelectionDragActive_ = true;
    messageSelectionAnchorPostId_ = anchorPostId;
    setMessageSelectionRange(currentPostId);
}

void ChatLogWidget::updateMessageSelectionDrag(const QString& currentPostId)
{
    if (!messageSelectionDragActive_ || currentPostId.isEmpty()) {
        return;
    }
    setMessageSelectionRange(currentPostId);
}

void ChatLogWidget::finishMessageSelectionDrag()
{
    messageSelectionDragActive_ = false;
}

void ChatLogWidget::setMessageSelectionRange(const QString& currentPostId)
{
    if (!postSource) {
        return;
    }
    const int anchor = postSource->indexOfPost(messageSelectionAnchorPostId_);
    const int current = postSource->indexOfPost(currentPostId);
    const PostSelectionRange range = postSelectionRange(anchor, current);
    if (!range.isValid()) {
        return;
    }

    selectedPostIds_.clear();
    selectedOwnPostIds_.clear();
    selectedFormattedPosts_.clear();
    for (int index = range.first; index <= range.last; ++index) {
        BackendPost* selected = postSource->postAt(index);
        if (!selected || selected->id.isEmpty()) {
            continue;
        }
        selectedPostIds_.insert(selected->id);
        if (selected->isOwnPost() && !selected->isDeleted) {
            selectedOwnPostIds_.insert(selected->id);
        }
    }
    applyMessageSelectionVisuals();
}

void ChatLogWidget::setMessagePostSelected(const QString& postId, bool selected)
{
    if (!messageSelectionMode_ || postId.isEmpty()) {
        return;
    }
    if (selected) {
        selectedPostIds_.insert(postId);
        cacheSelectedPost(postId);
        if (postSource) {
            const int index = postSource->indexOfPost(postId);
            BackendPost* post = index >= 0 ? postSource->postAt(index) : nullptr;
            if (post && post->isOwnPost() && !post->isDeleted) {
                selectedOwnPostIds_.insert(postId);
            }
        }
    } else {
        selectedPostIds_.remove(postId);
        selectedOwnPostIds_.remove(postId);
        selectedFormattedPosts_.remove(postId);
    }

    if (postSelectionShouldExit(selectedPostIds_.size())) {
        cancelMessageSelection();
        return;
    }
    applyMessageSelectionVisuals();
}

void ChatLogWidget::cacheSelectedPost(const QString& postId)
{
    PostWidget* widget = findPost(postId);
    if (!widget) {
        return;
    }
    selectedFormattedPosts_.insert(
        postId, widget->formatForClipboardSelection(PostWidget::entirePost));
    if (widget->post.isOwnPost() && !widget->post.isDeleted) {
        selectedOwnPostIds_.insert(postId);
    }
}

void ChatLogWidget::applyMessageSelectionVisuals()
{
    const Range range = materializedRange();
    if (range.isValid()) {
        for (int index = range.first; index <= range.last; ++index) {
            auto* widget = qobject_cast<PostWidget*>(itemWidget(index));
            if (!widget) {
                continue;
            }
            widget->setWholeMessageSelectionMode(messageSelectionMode_);
            widget->setWholeMessageSelected(selectedPostIds_.contains(widget->post.id));
            if (messageSelectionMode_) {
                widget->clearTextSelection();
                if (selectedPostIds_.contains(widget->post.id)) {
                    cacheSelectedPost(widget->post.id);
                }
            }
        }
        // LongListWidget captures/restores the viewport anchor for remeasurement;
        // widgets are retained and only their geometry changes for the checkbox gutter.
        itemsChanged(range.first, range.last);
    }
    updateSelectionToolbar();
}

void ChatLogWidget::cancelMessageSelection()
{
    if (!messageSelectionMode_ && selectedPostIds_.isEmpty()) {
        return;
    }
    messageSelectionMode_ = false;
    messageSelectionDragActive_ = false;
    messageSelectionAnchorPostId_.clear();
    selectedPostIds_.clear();
    selectedOwnPostIds_.clear();
    selectedFormattedPosts_.clear();
    applyMessageSelectionVisuals();
}

void ChatLogWidget::ensureSelectionToolbar()
{
    if (selectionToolbar_) {
        return;
    }
    selectionToolbar_ = new QFrame(viewport());
    selectionToolbar_->setFrameShape(QFrame::StyledPanel);
    selectionToolbar_->setAutoFillBackground(true);
    auto* layout = new QHBoxLayout(selectionToolbar_);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    selectionCountLabel_ = new QLabel(selectionToolbar_);
    layout->addWidget(selectionCountLabel_);

    selectionDeleteButton_ = new QPushButton(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/trash")), tr("Delete"), selectionToolbar_);
    selectionCopyButton_ = new QPushButton(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/copy")), tr("Copy"), selectionToolbar_);
    auto* cancelButton = new QPushButton(tr("Cancel"), selectionToolbar_);
    layout->addWidget(selectionDeleteButton_);
    layout->addWidget(selectionCopyButton_);
    layout->addWidget(cancelButton);

    connect(selectionDeleteButton_, &QPushButton::clicked,
            this, &ChatLogWidget::deleteSelectedOwnPosts);
    connect(selectionCopyButton_, &QPushButton::clicked,
            this, &ChatLogWidget::copySelectedPosts);
    connect(cancelButton, &QPushButton::clicked,
            this, &ChatLogWidget::cancelMessageSelection);
    selectionToolbar_->hide();
}

void ChatLogWidget::updateSelectionToolbar()
{
    if (!messageSelectionMode_) {
        if (selectionToolbar_) {
            selectionToolbar_->hide();
        }
        return;
    }
    ensureSelectionToolbar();
    selectionCountLabel_->setText(tr("%1 selected").arg(selectedPostIds_.size()));
    selectionDeleteButton_->setEnabled(!selectedOwnPostIds_.isEmpty());
    selectionCopyButton_->setEnabled(!selectedPostIds_.isEmpty());
    selectionToolbar_->adjustSize();
    positionSelectionToolbar();
    selectionToolbar_->show();
    selectionToolbar_->raise();
}

void ChatLogWidget::positionSelectionToolbar()
{
    if (!selectionToolbar_ || !viewport()) {
        return;
    }
    selectionToolbar_->adjustSize();
    const int x = std::max(8, (viewport()->width() - selectionToolbar_->width()) / 2);
    selectionToolbar_->move(x, 8);
}

void ChatLogWidget::copySelectedPosts()
{
    if (!postSource || selectedPostIds_.isEmpty()) {
        return;
    }
    QStringList blocks;
    for (int index = 0; index < postSource->itemCount(); ++index) {
        BackendPost* post = postSource->postAt(index);
        if (!post || !selectedPostIds_.contains(post->id)) {
            continue;
        }
        QString formatted = selectedFormattedPosts_.value(post->id);
        if (formatted.isEmpty()) {
            if (PostWidget* widget = findPost(post->id)) {
                formatted = widget->formatForClipboardSelection(PostWidget::entirePost);
            }
        }
        if (!formatted.isEmpty()) {
            blocks.push_back(formatted);
        }
    }
    if (!blocks.isEmpty()) {
        QApplication::clipboard()->setText(blocks.join(QStringLiteral("\n\n")));
    }
}

void ChatLogWidget::deleteSelectedOwnPosts()
{
    if (!backend) {
        return;
    }
    const QSet<QString> ownPosts = selectedOwnPostIds_;
    for (const QString& postId : ownPosts) {
        backend->deletePost(postId);
    }
    cancelMessageSelection();
}

'''
write(path, text[:pos] + selection_impl + text[pos:])

# --- Application-level mouse router: text drag crossing a post boundary promotes to post selection. ---
write("sources/chat-area/post/PostContextMenuRouter.cpp", r'''#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include "PostWidget.h"
#include "chat-area/ChatLogWidget.h"

namespace Mattermost {
namespace {

PostWidget* enclosingPost(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* post = qobject_cast<PostWidget*>(current)) {
            return post;
        }
    }
    return nullptr;
}

ChatLogWidget* enclosingLog(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* log = qobject_cast<ChatLogWidget*>(current)) {
            return log;
        }
    }
    return nullptr;
}

QPoint globalMousePosition(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

class PostContextMenuRouter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::ContextMenu) {
            PostWidget* postWidget = enclosingPost(widget);
            if (!postWidget || widget == postWidget) {
                return QObject::eventFilter(watched, event);
            }
            auto* contextEvent = static_cast<QContextMenuEvent*>(event);
            const QPoint postPosition = postWidget->mapFromGlobal(contextEvent->globalPos());
            QContextMenuEvent forwarded(contextEvent->reason(), postPosition,
                                        contextEvent->globalPos(), contextEvent->modifiers());
            QCoreApplication::sendEvent(postWidget, &forwarded);
            contextEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                PostWidget* post = enclosingPost(widget);
                ChatLogWidget* log = post ? enclosingLog(post) : nullptr;
                if (post && log && !log->isMessageSelectionMode()) {
                    dragOrigin_ = post;
                    dragLog_ = log;
                    wholePostDrag_ = false;
                } else {
                    clearDrag();
                }
            }
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseMove && dragOrigin_ && dragLog_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) {
                clearDrag();
                return QObject::eventFilter(watched, event);
            }

            const QPoint viewportPos = dragLog_->viewport()->mapFromGlobal(globalMousePosition(mouse));
            const int index = dragLog_->indexAtViewportPosition(viewportPos.y());
            auto* current = index >= 0
                ? qobject_cast<PostWidget*>(dragLog_->itemWidget(index)) : nullptr;
            if (!current) {
                return wholePostDrag_ ? true : QObject::eventFilter(watched, event);
            }

            if (!wholePostDrag_) {
                if (current == dragOrigin_ || dragOrigin_->getSelectedText().isEmpty()) {
                    return QObject::eventFilter(watched, event);
                }
                wholePostDrag_ = true;
                dragOrigin_->clearTextSelection();
                dragLog_->beginMessageSelectionDrag(dragOrigin_->post.id, current->post.id);
                return true;
            }

            dragLog_->updateMessageSelectionDrag(current->post.id);
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && dragOrigin_) {
                const bool consumed = wholePostDrag_;
                if (wholePostDrag_ && dragLog_) {
                    dragLog_->finishMessageSelectionDrag();
                }
                clearDrag();
                if (consumed) {
                    return true;
                }
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void clearDrag()
    {
        dragOrigin_.clear();
        dragLog_.clear();
        wholePostDrag_ = false;
    }

    QPointer<PostWidget> dragOrigin_;
    QPointer<ChatLogWidget> dragLog_;
    bool wholePostDrag_ = false;
};

void installPostContextMenuRouter()
{
    if (!qApp) {
        return;
    }
    auto* router = new PostContextMenuRouter(qApp);
    qApp->installEventFilter(router);
}

Q_COREAPP_STARTUP_FUNCTION(installPostContextMenuRouter)

} // namespace
} // namespace Mattermost
''')

# Wire icons used by the cleaned-up context menu.
write("img/copy.svg", '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="8" y="8" width="11" height="11" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/></svg>\n')
write("img/link.svg", '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M10 13a5 5 0 0 0 7.1.1l2-2a5 5 0 0 0-7.1-7.1L10.9 5"/><path d="M14 11a5 5 0 0 0-7.1-.1l-2 2A5 5 0 0 0 12 20l1.1-1"/></svg>\n')

replace_once(
    "resource.qrc",
    "        <file alias=\"message-balloon\">img/message-balloon.svg</file>\n",
    "        <file alias=\"message-balloon\">img/message-balloon.svg</file>\n"
    "        <file alias=\"edit\">img/edit.svg</file>\n"
    "        <file alias=\"copy\">img/copy.svg</file>\n"
    "        <file alias=\"link\">img/link.svg</file>\n")

# Pure regression policy for range expansion/shrinking and the zero-checkbox exit rule.
write("tests/PostSelectionPolicyTest.cpp", r'''#include <QtTest>

#include "chat-area/post/PostSelectionPolicy.h"

using namespace Mattermost;

class PostSelectionPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void forwardAndBackwardRangesAreInclusive()
    {
        QCOMPARE(postSelectionRange(4, 7).first, 4);
        QCOMPARE(postSelectionRange(4, 7).last, 7);
        QCOMPARE(postSelectionRange(7, 4).first, 4);
        QCOMPARE(postSelectionRange(7, 4).last, 7);
    }

    void movingBackTowardAnchorShrinksSelection()
    {
        const auto expanded = postSelectionRange(10, 15);
        const auto shrunk = postSelectionRange(10, 12);
        QVERIFY(expanded.contains(15));
        QVERIFY(!shrunk.contains(15));
        QVERIFY(shrunk.contains(10));
        QVERIFY(shrunk.contains(12));
    }

    void clearingLastCheckboxExitsMode()
    {
        QVERIFY(postSelectionShouldExit(0));
        QVERIFY(postSelectionShouldExit(-1));
        QVERIFY(!postSelectionShouldExit(1));
    }
};

QTEST_APPLESS_MAIN(PostSelectionPolicyTest)
#include "PostSelectionPolicyTest.moc"
''')

with Path("tests/CMakeLists.txt").open("a") as f:
    f.write(r'''

add_executable(post-selection-policy-test PostSelectionPolicyTest.cpp)
target_include_directories(post-selection-policy-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(post-selection-policy-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME post-selection-policy-test COMMAND post-selection-policy-test)
''')
