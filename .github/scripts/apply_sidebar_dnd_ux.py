from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


def write(path, text):
    p = ROOT / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:120]!r}")
    if text.count(old) != 1:
        raise RuntimeError(f"anchor not unique in {path}: {old[:120]!r}")
    write(path, text.replace(old, new, 1))


def replace_function(path, signature, replacement):
    text = read(path)
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"function not found in {path}: {signature}")
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError(f"opening brace not found in {path}: {signature}")
    depth = 0
    end = None
    for i in range(brace, len(text)):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        raise RuntimeError(f"closing brace not found in {path}: {signature}")
    write(path, text[:start] + replacement.rstrip() + text[end:])


# ---------------------------------------------------------------------------
# Transient tree-layout roles used by the drag UI.
# ---------------------------------------------------------------------------
replace_once(
    "sources/channel-tree/SidebarItem.h",
    "    DestinationRole,\n};",
    "    DestinationRole,\n\n"
    "    // Transient geometry roles used only while a ChannelTree drag is active.\n"
    "    // Values are view-local: gap roles are pixels, collapse is [0, 1].\n"
    "    DropGapBeforeRole,\n"
    "    DropGapAfterRole,\n"
    "    DragCollapseRole,\n};")

# ---------------------------------------------------------------------------
# Delegate: make the animated gap part of the real row geometry.
# ---------------------------------------------------------------------------
replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    "#include <QStyleOptionViewItem>\n",
    "#include <QStyleOptionViewItem>\n#include <QtMath>\n")

replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    "bool isSavedDestination(const QModelIndex& index)\n{\n    return index.data(SidebarItem::DestinationRole).toInt()\n        == SidebarItem::SavedDestination;\n}\n",
    "bool isSavedDestination(const QModelIndex& index)\n{\n    return index.data(SidebarItem::DestinationRole).toInt()\n        == SidebarItem::SavedDestination;\n}\n\n"
    "int transientGap(const QModelIndex& index, int role)\n"
    "{\n"
    "    return qMax(0, index.data(role).toInt());\n"
    "}\n\n"
    "qreal collapseProgress(const QModelIndex& index)\n"
    "{\n"
    "    return qBound<qreal>(0.0, index.data(SidebarItem::DragCollapseRole).toReal(), 1.0);\n"
    "}\n\n"
    "QStyleOptionViewItem contentOption(const QStyleOptionViewItem& option,\n"
    "                                   const QModelIndex& index)\n"
    "{\n"
    "    QStyleOptionViewItem result(option);\n"
    "    const int before = transientGap(index, SidebarItem::DropGapBeforeRole);\n"
    "    const int after = transientGap(index, SidebarItem::DropGapAfterRole);\n"
    "    result.rect.adjust(0, before, 0, -after);\n"
    "    return result;\n"
    "}\n\n"
    "void drawGapMarker(QPainter* painter, const QStyleOptionViewItem& option,\n"
    "                   const QModelIndex& index)\n"
    "{\n"
    "    const int before = transientGap(index, SidebarItem::DropGapBeforeRole);\n"
    "    const int after = transientGap(index, SidebarItem::DropGapAfterRole);\n"
    "    if (before <= 0 && after <= 0) {\n"
    "        return;\n"
    "    }\n\n"
    "    const int y = before > 0\n"
    "        ? option.rect.top() + before / 2\n"
    "        : option.rect.bottom() - after / 2;\n"
    "    QPen pen(option.palette.color(QPalette::Highlight));\n"
    "    pen.setWidth(2);\n"
    "    painter->save();\n"
    "    painter->setPen(pen);\n"
    "    painter->drawLine(option.rect.left() + 6, y, option.rect.right() - 6, y);\n"
    "    painter->restore();\n"
    "}\n")

replace_function(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    "QSize ChannelItemDelegate::sizeHint(const QStyleOptionViewItem& option,",
    r'''QSize ChannelItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
    QSize hint = QStyledItemDelegate::sizeHint(option, index);
    if (isTeamRow(index)) {
        hint.setHeight(0);
        return hint;
    }
    if (isConversationRow(index)) {
        hint.setHeight(ChannelRowHeight);
    }

    const int before = transientGap(index, SidebarItem::DropGapBeforeRole);
    const int after = transientGap(index, SidebarItem::DropGapAfterRole);
    const qreal collapse = collapseProgress(index);
    const int contentHeight = qMax(0, qRound(hint.height() * (1.0 - collapse)));
    hint.setHeight(contentHeight + before + after);
    return hint;
}''')

replace_function(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    "void ChannelItemDelegate::paint(QPainter* painter,",
    r'''void ChannelItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (isTeamRow(index)) {
        return;
    }

    const qreal collapse = collapseProgress(index);
    const QStyleOptionViewItem content = contentOption(option, index);
    if (content.rect.height() <= 0) {
        drawGapMarker(painter, option, index);
        return;
    }

    painter->save();
    painter->setOpacity(painter->opacity() * (1.0 - collapse));

    if (!isConversationRow(index)) {
        QStyledItemDelegate::paint(painter, content, index);
        painter->restore();
        drawGapMarker(painter, option, index);
        return;
    }

    QStyleOptionViewItem base(content);
    initStyleOption(&base, index);
    const QString text = base.text;
    QIcon icon = base.icon;
    const int type = channelType(index);
    const bool selected = content.state.testFlag(QStyle::State_Selected);

    if (isSavedDestination(index)) {
        const QColor iconColor = selected
            ? content.palette.color(QPalette::HighlightedText)
            : content.palette.color(QPalette::Text);
        icon = savedDestinationIcon(iconColor);
    }

    if (type == BackendChannel::groupChannel) {
        const QString channelId = index.data(SidebarItem::IdRole).toString();
        if (!channelId.isEmpty() && !requestedGroupChannels.contains(channelId)) {
            requestedGroupChannels.insert(channelId);
            if (QObject* owner = parent()) {
                QMetaObject::invokeMethod(owner,
                                          "ensureGroupChannelDisplayName",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, channelId));
            }
        }
    }

    if (icon.isNull()) {
        if (type == BackendChannel::groupChannel) {
            icon = ChannelIcons::groupConversation();
        } else if (type == BackendChannel::publicChannel
                   || type == BackendChannel::privateChannel) {
            icon = ChannelIcons::channel();
        }
    }

    base.text.clear();
    base.icon = QIcon();

    const QStyle* style = content.widget ? content.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &base, painter, content.widget);

    QRect contentRect = content.rect.adjusted(HorizontalMargin, 0, -HorizontalMargin, 0);
    int textLeft = contentRect.left();

    if (!icon.isNull()) {
        const QRect iconRect(textLeft,
                             contentRect.center().y() - AvatarSize / 2,
                             AvatarSize,
                             AvatarSize);
        const QPixmap pixmap = icon.pixmap(AvatarSize, AvatarSize);

        const QString status = type == BackendChannel::directChannel
            ? index.data(SidebarItem::PresenceRole).toString()
            : QString();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (!status.isEmpty()) {
            QPainterPath clip;
            clip.addEllipse(iconRect);
            painter->setClipPath(clip);
        }
        painter->drawPixmap(iconRect, pixmap);
        painter->restore();

        if (!status.isEmpty()) {
            const QRect statusRect(iconRect.right() - StatusSize + 3,
                                   iconRect.bottom() - StatusSize + 3,
                                   StatusSize,
                                   StatusSize);
            const QColor badgeBackground = selected
                ? content.palette.color(QPalette::Highlight)
                : content.palette.color(QPalette::Base);
            AvatarUtils::drawStatusBadge(painter, statusRect, status, badgeBackground);
        }

        textLeft = iconRect.right() + 1 + ItemSpacing;
    }

    int textRight = contentRect.right();
    if (index.data(SidebarItem::MutedRole).toBool()) {
        const QRect muteRect(textRight - MuteIconSize + 1,
                             contentRect.center().y() - MuteIconSize / 2,
                             MuteIconSize,
                             MuteIconSize);
        style->standardIcon(QStyle::SP_MediaVolumeMuted).paint(painter, muteRect);
        textRight = muteRect.left() - ItemSpacing;
    }

    QRect textRect(textLeft, contentRect.top(),
                   qMax(0, textRight - textLeft + 1), contentRect.height());
    const QFont font = base.font;
    painter->setFont(font);

    const bool muted = index.data(SidebarItem::MutedRole).toBool();
    const QColor textColor = selected
        ? content.palette.color(QPalette::HighlightedText)
        : (muted ? content.palette.color(QPalette::Disabled, QPalette::Text)
                 : content.palette.color(QPalette::Text));
    painter->setPen(textColor);

    const QString elided = QFontMetrics(font).elidedText(text, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
    painter->restore();
    drawGapMarker(painter, option, index);
}''')

# Branch disclosure arrows must stay centered in the non-gap content row.
replace_function(
    "sources/channel-tree/ChannelTreeBranches.cpp",
    "void ChannelTree::drawBranches(QPainter* painter,",
    r'''void ChannelTree::drawBranches(QPainter* painter, const QRect& rect,
                               const QModelIndex& index) const
{
    if (!painter || !index.isValid() || !model()->hasChildren(index)) {
        return;
    }

    QRect contentRect = rect;
    contentRect.adjust(0,
                       qMax(0, index.data(SidebarItem::DropGapBeforeRole).toInt()),
                       0,
                       -qMax(0, index.data(SidebarItem::DropGapAfterRole).toInt()));
    if (contentRect.height() <= 0) {
        return;
    }

    QStyleOption option;
    option.initFrom(this);

    const int extent = qMin(indentation(), contentRect.height());
    option.rect = QRect(0, 0, extent, extent);
    option.rect.moveCenter(contentRect.center());
    if (layoutDirection() == Qt::LeftToRight) {
        option.rect.moveLeft(contentRect.right() - extent + 1);
    } else {
        option.rect.moveRight(contentRect.left() + extent - 1);
    }

    style()->drawPrimitive(isExpanded(index)
                               ? QStyle::PE_IndicatorArrowDown
                               : QStyle::PE_IndicatorArrowRight,
                           &option, painter, this);
}''')

# ---------------------------------------------------------------------------
# SidebarService: local optimistic state + explicit error callback.  DnD calls
# use storeResponse=false so out-of-order PUT responses cannot overwrite a newer
# local projection.
# ---------------------------------------------------------------------------
replace_once(
    "sources/backend/SidebarService.h",
    "    SidebarTeamState* teamState(const QString& teamId);\n\n",
    "    SidebarTeamState* teamState(const QString& teamId);\n"
    "    void applyLocalTeamState(const QString& teamId, SidebarTeamState state);\n\n")

replace_once(
    "sources/backend/SidebarService.h",
    "    void updateCategory(const SidebarCategory& category,\n"
    "                        std::function<void(const SidebarCategory&)> callback = {});\n"
    "    void updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,\n"
    "                          std::function<void(const SidebarTeamState&)> callback = {});\n"
    "    void updateCategoryOrder(const QString& teamId, const QStringList& order,\n"
    "                             std::function<void()> callback = {});",
    "    void updateCategory(const SidebarCategory& category,\n"
    "                        std::function<void(const SidebarCategory&)> callback = {},\n"
    "                        std::function<void()> errorCallback = {},\n"
    "                        bool storeResponse = true);\n"
    "    void updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,\n"
    "                          std::function<void(const SidebarTeamState&)> callback = {},\n"
    "                          std::function<void()> errorCallback = {},\n"
    "                          bool storeResponse = true);\n"
    "    void updateCategoryOrder(const QString& teamId, const QStringList& order,\n"
    "                             std::function<void()> callback = {},\n"
    "                             std::function<void()> errorCallback = {},\n"
    "                             bool storeResponse = true);")

replace_once(
    "sources/backend/SidebarService.cpp",
    "SidebarTeamState* SidebarService::teamState(const QString& teamId)\n"
    "{\n"
    "    auto it = sidebarByTeam.find(teamId);\n"
    "    return it == sidebarByTeam.end() ? nullptr : &it.value();\n"
    "}\n",
    "SidebarTeamState* SidebarService::teamState(const QString& teamId)\n"
    "{\n"
    "    auto it = sidebarByTeam.find(teamId);\n"
    "    return it == sidebarByTeam.end() ? nullptr : &it.value();\n"
    "}\n\n"
    "void SidebarService::applyLocalTeamState(const QString& teamId, SidebarTeamState state)\n"
    "{\n"
    "    if (teamId.isEmpty()) {\n"
    "        return;\n"
    "    }\n"
    "    sidebarByTeam.insert(teamId, std::move(state));\n"
    "    emit categoriesChanged(teamId);\n"
    "}\n")

replace_function(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::updateCategory(const SidebarCategory& category,",
    r'''void SidebarService::updateCategory(const SidebarCategory& category,
                                    std::function<void(const SidebarCategory&)> callback,
                                    std::function<void()> errorCallback,
                                    bool storeResponse)
{
    NetworkRequest request(categoriesPath(category.teamId) + QLatin1Char('/') + category.id);
    httpConnector.put(request, QByteArrayCreator(category.toJson()),
                      HttpResponseCallback([this, teamId = category.teamId, callback,
                                            errorCallback, storeResponse](
                                               QVariant status, const QJsonDocument& doc) {
        if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
            if (errorCallback) {
                errorCallback();
            }
            return;
        }
        SidebarCategory updated = SidebarCategory::fromJson(doc.object());
        if (updated.id.isEmpty()) {
            if (errorCallback) {
                errorCallback();
            }
            return;
        }
        if (storeResponse) {
            sidebarByTeam[teamId].categories.insert(updated.id, updated);
            emit categoriesChanged(teamId);
            if (callback) {
                callback(sidebarByTeam[teamId].categories[updated.id]);
            }
        } else if (callback) {
            callback(updated);
        }
    }));
}''')

replace_function(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,",
    r'''void SidebarService::updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,
                                      std::function<void(const SidebarTeamState&)> callback,
                                      std::function<void()> errorCallback,
                                      bool storeResponse)
{
    QJsonArray payload;
    for (const auto& category : categories) {
        payload.push_back(category.toJson());
    }

    NetworkRequest request(categoriesPath(teamId));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback, errorCallback,
                                            storeResponse](QVariant status,
                                                           const QJsonDocument& doc) {
        if (status.toInt() != QNetworkReply::NoError || !doc.isArray()) {
            if (errorCallback) {
                errorCallback();
            }
            return;
        }

        SidebarTeamState responseState = sidebarByTeam.value(teamId);
        for (const auto& value : doc.array()) {
            SidebarCategory updated = SidebarCategory::fromJson(value.toObject());
            if (!updated.id.isEmpty()) {
                responseState.categories.insert(updated.id, std::move(updated));
            }
        }

        if (storeResponse) {
            sidebarByTeam.insert(teamId, std::move(responseState));
            emit categoriesChanged(teamId);
            if (callback) {
                callback(sidebarByTeam[teamId]);
            }
        } else if (callback) {
            callback(responseState);
        }
    }));
}''')

replace_function(
    "sources/backend/SidebarService.cpp",
    "void SidebarService::updateCategoryOrder(const QString& teamId, const QStringList& order,",
    r'''void SidebarService::updateCategoryOrder(const QString& teamId, const QStringList& order,
                                         std::function<void()> callback,
                                         std::function<void()> errorCallback,
                                         bool storeResponse)
{
    QJsonArray payload;
    for (const auto& categoryId : order) {
        payload.push_back(categoryId);
    }

    NetworkRequest request(categoriesPath(teamId) + QStringLiteral("/order"));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback, errorCallback,
                                            storeResponse](QVariant status,
                                                           const QJsonDocument& doc) {
        if (status.toInt() != QNetworkReply::NoError || !doc.isArray()) {
            if (errorCallback) {
                errorCallback();
            }
            return;
        }
        QStringList updatedOrder;
        for (const auto& value : doc.array()) {
            updatedOrder.push_back(value.toString());
        }
        if (storeResponse) {
            sidebarByTeam[teamId].order = updatedOrder;
            emit categoriesChanged(teamId);
        }
        if (callback) {
            callback();
        }
    }));
}''')

# ---------------------------------------------------------------------------
# ChannelTree interface/state.
# ---------------------------------------------------------------------------
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "#include <QMap>\n#include <QSet>\n#include <QTreeWidget>\n",
    "#include <QMap>\n#include <QPersistentModelIndex>\n#include <QSet>\n#include <QTreeWidget>\n#include <QVector>\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "class QDragMoveEvent;\n",
    "class QDragLeaveEvent;\nclass QDragMoveEvent;\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "class QTreeWidgetItem;\n",
    "class QTreeWidgetItem;\nclass QVariantAnimation;\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid mousePressEvent(QMouseEvent* event) override;\n\tvoid dragMoveEvent(QDragMoveEvent* event) override;\n\tvoid dropEvent(QDropEvent* event) override;",
    "\tvoid mousePressEvent(QMouseEvent* event) override;\n"
    "    void startDrag(Qt::DropActions supportedActions) override;\n"
    "    void dragLeaveEvent(QDragLeaveEvent* event) override;\n"
    "\tvoid dragMoveEvent(QDragMoveEvent* event) override;\n"
    "\tvoid dropEvent(QDropEvent* event) override;")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid renderTeamSidebar(Backend& backend, TeamItem& teamItem,\n\t                       const SidebarTeamState& state);\n",
    "\tvoid renderTeamSidebar(Backend& backend, TeamItem& teamItem,\n"
    "\t                       const SidebarTeamState& state);\n"
    "    void reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,\n"
    "                              const SidebarTeamState& state);\n"
    "    void destroySidebarRow(QTreeWidgetItem* item);\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "\tvoid syncCategoryChannels(QTreeWidgetItem* firstCategory, QTreeWidgetItem* secondCategory = nullptr);\n"
    "\tvoid syncCategoryOrder(QTreeWidgetItem* teamItem);\n"
    "\tQStringList channelIds(QTreeWidgetItem* categoryItem) const;\n",
    "")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,\n"
    "                                   QTreeWidgetItem*& targetCategoryItem,\n"
    "                                   bool& afterTarget) const;\n",
    "    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,\n"
    "                                   QTreeWidgetItem*& targetCategoryItem,\n"
    "                                   bool& afterTarget) const;\n"
    "    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,\n"
    "                           bool gapAfter);\n"
    "    void clearDropGap(bool animate);\n"
    "    void resetDragVisuals(bool animate);\n"
    "    void ensureDragSourceVisuals(QTreeWidgetItem* source);\n"
    "    void animateSourceCollapse(qreal target);\n"
    "    void animateDropGap(const QPersistentModelIndex& target, bool after, int extent);\n"
    "    QTreeWidgetItem* channelDropGapAnchor(QTreeWidgetItem* source,\n"
    "                                            QTreeWidgetItem* targetCategoryItem,\n"
    "                                            const QString& targetChannelId,\n"
    "                                            bool afterTarget, bool& gapAfter) const;\n"
    "    QTreeWidgetItem* categoryDropGapAnchor(QTreeWidgetItem* targetCategoryItem,\n"
    "                                             bool afterTarget, bool& gapAfter) const;\n")
replace_once(
    "sources/channel-tree/ChannelTree.h",
    "    bool                                personalUserConnected = false;\n",
    "    bool                                personalUserConnected = false;\n"
    "    QVariantAnimation*                  sourceCollapseAnimation = nullptr;\n"
    "    QVariantAnimation*                  dropGapAnimation = nullptr;\n"
    "    QVector<QPersistentModelIndex>      dragSourceIndexes;\n"
    "    QPersistentModelIndex               dragGapIndex;\n"
    "    int                                 draggedRowExtent = 0;\n"
    "    QMap<QString, quint64>              sidebarMutationGeneration;\n")

# Disable Qt's thin insertion line; our structural gap is the indicator.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "    setDropIndicatorShown(true);",
    "    setDropIndicatorShown(false);")

# Gap-aware midpoint so an opening gap does not make the target oscillate.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "QPoint dropEventPosition(const QDropEvent* event)\n{\n#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)\n    return event->position().toPoint();\n#else\n    return event->pos();\n#endif\n}\n",
    "QPoint dropEventPosition(const QDropEvent* event)\n"
    "{\n"
    "#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)\n"
    "    return event->position().toPoint();\n"
    "#else\n"
    "    return event->pos();\n"
    "#endif\n"
    "}\n\n"
    "QRect dragContentRect(const QTreeWidgetItem* item, QRect rect)\n"
    "{\n"
    "    if (!item) {\n"
    "        return rect;\n"
    "    }\n"
    "    rect.adjust(0, qMax(0, item->data(0, SidebarItem::DropGapBeforeRole).toInt()),\n"
    "                0, -qMax(0, item->data(0, SidebarItem::DropGapAfterRole).toInt()));\n"
    "    return rect;\n"
    "}\n")

replace_function(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::renderTeamSidebar(Backend& backend, TeamItem& teamItem,",
    r'''void ChannelTree::renderTeamSidebar(Backend& backend, TeamItem& teamItem,
                                    const SidebarTeamState& state)
{
    reconcileTeamSidebar(backend, teamItem, state);
}''')

replace_function(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::moveChannel(ChannelItem* item,",
    r'''void ChannelTree::moveChannel(ChannelItem* item,
                              const QString& targetCategoryId,
                              const QString& targetChannelId,
                              bool afterTarget,
                              bool explicitPosition)
{
    if (!backendForSidebar || !item || !item->parent() || targetCategoryId.isEmpty()) {
        return;
    }

    QTreeWidgetItem* sourceCategoryItem = item->parent();
    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    auto& sidebar = SidebarService::instance(*backendForSidebar);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state) {
        return;
    }

    const QString sourceCategoryId = sourceCategoryItem->data(0, ItemIdRole).toString();
    const QString channelId = item->data(0, ItemIdRole).toString();
    const SidebarCategory* sourceCategory = state->category(sourceCategoryId);
    const SidebarCategory* targetCategory = state->category(targetCategoryId);
    if (!sourceCategory || !targetCategory || targetCategory->teamId != teamId
        || channelId.isEmpty()) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    SidebarCategory* sourceUpdate = optimistic.category(sourceCategoryId);
    SidebarCategory* targetUpdate = optimistic.category(targetCategoryId);
    if (!sourceUpdate || !targetUpdate) {
        return;
    }

    bool changed = false;
    const bool sameCategory = sourceCategoryId == targetCategoryId;
    if (sameCategory) {
        changed = reorderSidebarChannel(sourceUpdate->channelIds, channelId,
                                        targetChannelId, afterTarget);
        if (changed) {
            sourceUpdate->sorting = QStringLiteral("manual");
        }
    } else {
        changed = moveSidebarChannel(sourceUpdate->channelIds, targetUpdate->channelIds,
                                     channelId, targetChannelId, afterTarget);
        if (changed && explicitPosition) {
            targetUpdate->sorting = QStringLiteral("manual");
        }
    }
    if (!changed) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamId];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamId, nullptr)) {
        if (const SidebarTeamState* local = sidebar.teamState(teamId)) {
            reconcileTeamSidebar(*backendForSidebar, *teamItem, *local);
        }
    }

    QPointer<ChannelTree> guard(this);
    auto verify = [guard, teamId, mutation] {
        if (!guard || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        guard->refreshSidebarTeam(teamId);
    };
    auto rollback = [guard, teamId, mutation, before] {
        if (!guard || !guard->backendForSidebar
            || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        auto& currentSidebar = SidebarService::instance(*guard->backendForSidebar);
        currentSidebar.applyLocalTeamState(teamId, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
            guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, before);
        }
        // The rollback is immediate; the GET is only an authoritative check and
        // is reconciled incrementally when it arrives.
        guard->refreshSidebarTeam(teamId);
    };

    if (sameCategory) {
        const SidebarCategory payload = *optimistic.category(sourceCategoryId);
        sidebar.updateCategory(
            payload,
            [verify](const SidebarCategory&) { verify(); },
            rollback,
            false);
    } else {
        const QVector<SidebarCategory> payload {
            *optimistic.category(sourceCategoryId),
            *optimistic.category(targetCategoryId),
        };
        sidebar.updateCategories(
            teamId, payload,
            [verify](const SidebarTeamState&) { verify(); },
            rollback,
            false);
    }
}''')

replace_function(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::moveCategory(QTreeWidgetItem* item,",
    r'''void ChannelTree::moveCategory(QTreeWidgetItem* item,
                               const QString& targetCategoryId,
                               bool afterTarget)
{
    if (!backendForSidebar || !item || targetCategoryId.isEmpty()
        || item->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    auto& sidebar = SidebarService::instance(*backendForSidebar);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state || categoryId.isEmpty() || !state->category(categoryId)
        || !state->category(targetCategoryId)) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    if (!reorderSidebarCategory(optimistic.order, categoryId,
                                targetCategoryId, afterTarget)) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamId];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamId, nullptr)) {
        reconcileTeamSidebar(*backendForSidebar, *teamItem, optimistic);
    }

    QPointer<ChannelTree> guard(this);
    auto rollback = [guard, teamId, mutation, before] {
        if (!guard || !guard->backendForSidebar
            || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        auto& currentSidebar = SidebarService::instance(*guard->backendForSidebar);
        currentSidebar.applyLocalTeamState(teamId, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
            guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, before);
        }
        guard->refreshSidebarTeam(teamId);
    };

    sidebar.updateCategoryOrder(
        teamId, optimistic.order,
        [guard, teamId, mutation] {
            if (guard && guard->sidebarMutationGeneration.value(teamId) == mutation) {
                guard->refreshSidebarTeam(teamId);
            }
        },
        rollback,
        false);
}''')

# Stabilize before/after probing against an already-open structural gap.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "        const QRect rect = visualItemRect(target);\n"
    "        afterTarget = rect.isValid() && pos.y() >= rect.center().y();",
    "        const QRect rect = dragContentRect(target, visualItemRect(target));\n"
    "        afterTarget = rect.isValid() && pos.y() >= rect.center().y();")
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "    const QRect rect = visualItemRect(target);\n"
    "    afterTarget = rect.isValid() && pos.y() >= rect.center().y();\n"
    "    return true;",
    "    const QRect rect = dragContentRect(target, visualItemRect(target));\n"
    "    afterTarget = rect.isValid() && pos.y() >= rect.center().y();\n"
    "    return true;")

replace_function(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::dragMoveEvent(QDragMoveEvent* event)",
    r'''void ChannelTree::dragMoveEvent(QDragMoveEvent* event)
{
    QTreeWidget::dragMoveEvent(event);
    const auto selected = selectedItems();
    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            clearDropGap(true);
            event->ignore();
            return;
        }
        bool gapAfter = afterTarget;
        QTreeWidgetItem* anchor = categoryDropGapAnchor(
            targetCategoryItem, afterTarget, gapAfter);
        updateDragVisuals(source, anchor, gapAfter);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        clearDropGap(true);
        event->ignore();
        return;
    }

    bool gapAfter = afterTarget;
    QTreeWidgetItem* anchor = channelDropGapAnchor(
        source, targetCategoryItem, targetChannelId, afterTarget, gapAfter);
    updateDragVisuals(source, anchor, gapAfter);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}''')

replace_function(
    "sources/channel-tree/ChannelTree.cpp",
    "void ChannelTree::dropEvent(QDropEvent* event)",
    r'''void ChannelTree::dropEvent(QDropEvent* event)
{
    const auto selected = selectedItems();
    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();
    const QPoint pos = dropEventPosition(event);

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            resetDragVisuals(true);
            event->ignore();
            return;
        }

        const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
        resetDragVisuals(false);
        moveCategory(source, targetCategoryId, afterTarget);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        resetDragVisuals(true);
        event->ignore();
        return;
    }

    auto* channelItem = static_cast<ChannelItem*>(source);
    const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
    resetDragVisuals(false);
    moveChannel(channelItem, targetCategoryId, targetChannelId,
                afterTarget, !targetChannelId.isEmpty());
    event->setDropAction(Qt::MoveAction);
    event->accept();
}''')

# ---------------------------------------------------------------------------
# Incremental tree reconciliation.  Existing rows (and their ChatArea) survive
# server refreshes; a row moved between categories is reparented, not recreated.
# ---------------------------------------------------------------------------
write("sources/channel-tree/ChannelTreeReconcile.cpp", r'''/**
 * @file ChannelTreeReconcile.cpp
 * @brief Incremental reconciliation of the server sidebar with the live tree.
 */

#include "ChannelTree.h"

#include <QSet>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {
namespace {

QTreeWidgetItem* findChannelChild(QTreeWidgetItem* category, const QString& channelId)
{
    if (!category) {
        return nullptr;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* row = category->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel
            && row->data(0, SidebarItem::IdRole).toString() == channelId) {
            return row;
        }
    }
    return nullptr;
}

QTreeWidgetItem* findVirtualChild(QTreeWidgetItem* category, int destination)
{
    if (!category) {
        return nullptr;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* row = category->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::VirtualDestination
            && row->data(0, SidebarItem::DestinationRole).toInt() == destination) {
            return row;
        }
    }
    return nullptr;
}

void moveChild(QTreeWidgetItem& parent, QTreeWidgetItem* child, int index)
{
    if (!child || child->parent() != &parent) {
        return;
    }
    const int current = parent.indexOfChild(child);
    if (current < 0 || current == index) {
        return;
    }
    parent.takeChild(current);
    parent.insertChild(index, child);
}

} // namespace

void ChannelTree::destroySidebarRow(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == ChannelItemKind) {
        removeChannelToItem(item->data(0, ItemIdRole).toString(), item);
    }

    if (kind == ChannelItemKind || kind == VirtualDestinationItemKind) {
        ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
        if (chatArea) {
            if (chatAreaStackedWidget) {
                chatAreaStackedWidget->removeWidget(chatArea);
            }
            delete chatArea;
        }
    }
    delete item;
}

void ChannelTree::reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,
                                       const SidebarTeamState& state)
{
    // A server reconciliation is structural. Never leave transient drag geometry
    // attached to rows that may be moved or removed underneath it.
    resetDragVisuals(false);
    renderingSidebar = true;

    auto& sidebar = SidebarService::instance(backend);
    BackendChannel* personalChannel = backend.getStorage().getDirectChannelByUserId(
        backend.getLoginUser().id);

    QMap<QString, QStringList> desiredChannels;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category) {
            continue;
        }
        QStringList ids = sidebar.visibleChannelIds(*category);
        if (category->type == QStringLiteral("favorites") && personalChannel) {
            ids.removeAll(personalChannel->id);
        }
        desiredChannels.insert(categoryId, std::move(ids));
    }

    QMap<QString, QTreeWidgetItem*> categories;
    for (int i = 0; i < teamItem.childCount(); ++i) {
        QTreeWidgetItem* category = teamItem.child(i);
        if (!category || category->data(0, ItemKindRole).toInt() != CategoryItemKind) {
            continue;
        }
        categories.insert(category->data(0, ItemIdRole).toString(), category);
    }

    // Rows which no longer belong to their current category are reusable.  Keep
    // them alive so a server-side move preserves the ChannelItem and ChatArea.
    QMap<QString, QList<QTreeWidgetItem*>> movableChannels;
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        const QSet<QString> desired(desiredChannels.value(it.key()).cbegin(),
                                    desiredChannels.value(it.key()).cend());
        QTreeWidgetItem* category = it.value();
        for (int row = 0; category && row < category->childCount(); ++row) {
            QTreeWidgetItem* child = category->child(row);
            if (!child || child->data(0, ItemKindRole).toInt() != ChannelItemKind) {
                continue;
            }
            const QString channelId = child->data(0, ItemIdRole).toString();
            if (!desired.contains(channelId)) {
                movableChannels[channelId].push_back(child);
            }
        }
    }

    struct ActiveCategory {
        QTreeWidgetItem* item = nullptr;
        const SidebarCategory* category = nullptr;
        int desiredRows = 0;
    };
    QVector<ActiveCategory> active;

    int categoryIndex = 0;
    QSet<QString> seenCategories;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category || seenCategories.contains(categoryId)) {
            continue;
        }
        seenCategories.insert(categoryId);

        QTreeWidgetItem* categoryItem = categories.value(categoryId, nullptr);
        if (!categoryItem) {
            categoryItem = createCategoryItem(
                teamItem, category->id, categoryDisplayName(*category), category->collapsed);
            categories.insert(categoryId, categoryItem);
        }

        const int currentIndex = teamItem.indexOfChild(categoryItem);
        if (currentIndex >= 0 && currentIndex != categoryIndex) {
            teamItem.takeChild(currentIndex);
            teamItem.insertChild(categoryIndex, categoryItem);
        }
        categoryItem->setText(0, categoryDisplayName(*category));
        categoryItem->setData(0, ItemTeamIdRole, teamItem.teamId);
        categoryItem->setExpanded(!category->collapsed);

        active.push_back(ActiveCategory {categoryItem, category, 0});
        ++categoryIndex;
    }

    // Place every desired row first. Surplus rows are only destroyed after all
    // categories had a chance to claim them.
    for (ActiveCategory& entry : active) {
        QTreeWidgetItem* categoryItem = entry.item;
        int rowIndex = 0;
        const bool favorites = entry.category->type == QStringLiteral("favorites");

        if (favorites) {
            QTreeWidgetItem* personal = findVirtualChild(
                categoryItem, SidebarItem::PersonalDestination);
            if (!personal) {
                personal = createPersonalItem(backend, teamItem, *categoryItem);
            }
            moveChild(*categoryItem, personal, rowIndex++);

            QTreeWidgetItem* saved = findVirtualChild(
                categoryItem, SidebarItem::SavedDestination);
            if (!saved) {
                saved = createSavedItem(backend, teamItem, *categoryItem);
            }
            moveChild(*categoryItem, saved, rowIndex++);
        }

        for (const QString& channelId : desiredChannels.value(entry.category->id)) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
            if (!channel) {
                continue;
            }

            QTreeWidgetItem* row = findChannelChild(categoryItem, channelId);
            if (!row) {
                auto& candidates = movableChannels[channelId];
                while (!candidates.isEmpty() && !row) {
                    QTreeWidgetItem* candidate = candidates.takeFirst();
                    if (!candidate || !candidate->parent()) {
                        continue;
                    }
                    QTreeWidgetItem* oldParent = candidate->parent();
                    const int oldIndex = oldParent->indexOfChild(candidate);
                    if (oldIndex >= 0) {
                        oldParent->takeChild(oldIndex);
                        categoryItem->insertChild(rowIndex, candidate);
                        row = candidate;
                    }
                }
            }
            if (!row) {
                row = createChannelItem(backend, teamItem, *categoryItem, *channel);
            }
            moveChild(*categoryItem, row, rowIndex++);
        }
        entry.desiredRows = rowIndex;
    }

    for (const ActiveCategory& entry : active) {
        while (entry.item->childCount() > entry.desiredRows) {
            QTreeWidgetItem* extra = entry.item->takeChild(entry.desiredRows);
            destroySidebarRow(extra);
        }
    }

    // Desired categories were moved to the front in exact order; anything left
    // after them no longer exists in the authoritative state.
    while (teamItem.childCount() > active.size()) {
        QTreeWidgetItem* staleCategory = teamItem.takeChild(active.size());
        while (staleCategory && staleCategory->childCount() > 0) {
            destroySidebarRow(staleCategory->takeChild(0));
        }
        delete staleCategory;
    }

    teamItem.setExpanded(true);
    renderingSidebar = false;
}

} // namespace Mattermost
''')

# categoryDisplayName lives in ChannelTree.cpp's anonymous namespace; expose a
# small local equivalent in the reconciliation TU.
replace_once(
    "sources/channel-tree/ChannelTreeReconcile.cpp",
    "namespace {\n\nQTreeWidgetItem* findChannelChild",
    "namespace {\n\n"
    "QString categoryDisplayName(const SidebarCategory& category)\n"
    "{\n"
    "    if (!category.displayName.isEmpty()) {\n"
    "        return category.displayName;\n"
    "    }\n"
    "    if (category.type == QStringLiteral(\"favorites\")) {\n"
    "        return QStringLiteral(\"Favorites\");\n"
    "    }\n"
    "    if (category.type == QStringLiteral(\"channels\")) {\n"
    "        return QStringLiteral(\"Channels\");\n"
    "    }\n"
    "    if (category.type == QStringLiteral(\"direct_messages\")) {\n"
    "        return QStringLiteral(\"Direct Messages\");\n"
    "    }\n"
    "    return QStringLiteral(\"Category\");\n"
    "}\n\n"
    "QTreeWidgetItem* findChannelChild")

# ---------------------------------------------------------------------------
# Drag geometry/animation controller for the widget tree.
# ---------------------------------------------------------------------------
write("sources/channel-tree/ChannelTreeDragVisuals.cpp", r'''/**
 * @file ChannelTreeDragVisuals.cpp
 * @brief Animated structural displacement for ChannelTree drag and drop.
 */

#include "ChannelTree.h"

#include <QDragLeaveEvent>
#include <QEasingCurve>
#include <QVariantAnimation>

namespace Mattermost {
namespace {

constexpr int DragAnimationMs = 160;

void stopAnimation(QVariantAnimation*& animation)
{
    if (!animation) {
        return;
    }
    animation->stop();
    animation->deleteLater();
    animation = nullptr;
}

qreal collapseValue(const QPersistentModelIndex& index)
{
    return index.isValid()
        ? qBound<qreal>(0.0, index.data(SidebarItem::DragCollapseRole).toReal(), 1.0)
        : 0.0;
}

int gapValue(const QPersistentModelIndex& index, int role)
{
    return index.isValid() ? qMax(0, index.data(role).toInt()) : 0;
}

} // namespace

void ChannelTree::startDrag(Qt::DropActions supportedActions)
{
    QTreeWidget::startDrag(supportedActions);
    if (!dragSourceIndexes.isEmpty() || dragGapIndex.isValid()) {
        resetDragVisuals(true);
    }
}

void ChannelTree::dragLeaveEvent(QDragLeaveEvent* event)
{
    QTreeWidget::dragLeaveEvent(event);
    clearDropGap(true);
}

void ChannelTree::ensureDragSourceVisuals(QTreeWidgetItem* source)
{
    if (!source) {
        return;
    }
    const QPersistentModelIndex sourceIndex(indexFromItem(source, 0));
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front() == sourceIndex) {
        return;
    }

    resetDragVisuals(false);
    draggedRowExtent = 0;

    auto append = [this](QTreeWidgetItem* row) {
        if (!row || row->isHidden()) {
            return;
        }
        const QModelIndex index = indexFromItem(row, 0);
        const QRect rect = visualItemRect(row);
        if (!index.isValid() || !rect.isValid() || rect.height() <= 0) {
            return;
        }
        dragSourceIndexes.push_back(QPersistentModelIndex(index));
        draggedRowExtent += rect.height();
    };

    append(source);
    if (source->data(0, ItemKindRole).toInt() == CategoryItemKind && source->isExpanded()) {
        for (int i = 0; i < source->childCount(); ++i) {
            append(source->child(i));
        }
    }

    if (draggedRowExtent <= 0) {
        draggedRowExtent = visualItemRect(source).height();
    }
    animateSourceCollapse(1.0);
}

void ChannelTree::animateSourceCollapse(qreal target)
{
    stopAnimation(sourceCollapseAnimation);
    if (dragSourceIndexes.isEmpty()) {
        return;
    }

    QVector<qreal> starts;
    starts.reserve(dragSourceIndexes.size());
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        starts.push_back(collapseValue(index));
    }

    auto* animation = new QVariantAnimation(this);
    sourceCollapseAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    const auto indexes = dragSourceIndexes;
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, indexes, starts, target](const QVariant& value) {
        if (sourceCollapseAnimation != animation) {
            return;
        }
        const qreal progress = value.toReal();
        for (int i = 0; i < indexes.size(); ++i) {
            if (!indexes[i].isValid()) {
                continue;
            }
            const qreal current = starts[i] + (target - starts[i]) * progress;
            model()->setData(indexes[i], current, SidebarItem::DragCollapseRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, target] {
        if (sourceCollapseAnimation != animation) {
            return;
        }
        sourceCollapseAnimation = nullptr;
        animation->deleteLater();
        if (target <= 0.0) {
            dragSourceIndexes.clear();
            draggedRowExtent = 0;
        }
    });
    animation->start();
}

void ChannelTree::animateDropGap(const QPersistentModelIndex& target, bool after, int extent)
{
    stopAnimation(dropGapAnimation);

    const QPersistentModelIndex old = dragGapIndex;
    const int oldBefore = gapValue(old, SidebarItem::DropGapBeforeRole);
    const int oldAfter = gapValue(old, SidebarItem::DropGapAfterRole);
    const int newBefore = gapValue(target, SidebarItem::DropGapBeforeRole);
    const int newAfter = gapValue(target, SidebarItem::DropGapAfterRole);
    const int wantedBefore = target.isValid() && !after ? qMax(0, extent) : 0;
    const int wantedAfter = target.isValid() && after ? qMax(0, extent) : 0;

    dragGapIndex = target;
    auto* animation = new QVariantAnimation(this);
    dropGapAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, old, target, oldBefore, oldAfter,
             newBefore, newAfter, wantedBefore, wantedAfter](const QVariant& value) {
        if (dropGapAnimation != animation) {
            return;
        }
        const qreal progress = value.toReal();
        if (old.isValid() && old != target) {
            model()->setData(old, qRound(oldBefore * (1.0 - progress)),
                             SidebarItem::DropGapBeforeRole);
            model()->setData(old, qRound(oldAfter * (1.0 - progress)),
                             SidebarItem::DropGapAfterRole);
        }
        if (target.isValid()) {
            const int before = qRound(newBefore + (wantedBefore - newBefore) * progress);
            const int afterValue = qRound(newAfter + (wantedAfter - newAfter) * progress);
            model()->setData(target, before, SidebarItem::DropGapBeforeRole);
            model()->setData(target, afterValue, SidebarItem::DropGapAfterRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, old, target] {
        if (dropGapAnimation != animation) {
            return;
        }
        if (old.isValid() && old != target) {
            model()->setData(old, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(old, 0, SidebarItem::DropGapAfterRole);
        }
        dropGapAnimation = nullptr;
        animation->deleteLater();
        if (!target.isValid()) {
            dragGapIndex = QPersistentModelIndex();
        }
    });
    animation->start();
}

void ChannelTree::updateDragVisuals(QTreeWidgetItem* source,
                                    QTreeWidgetItem* gapAnchor,
                                    bool gapAfter)
{
    ensureDragSourceVisuals(source);
    if (!gapAnchor || draggedRowExtent <= 0) {
        clearDropGap(true);
        return;
    }
    animateDropGap(QPersistentModelIndex(indexFromItem(gapAnchor, 0)),
                   gapAfter, draggedRowExtent);
}

void ChannelTree::clearDropGap(bool animate)
{
    if (!dragGapIndex.isValid()) {
        return;
    }
    if (animate) {
        animateDropGap(QPersistentModelIndex(), false, 0);
        return;
    }

    stopAnimation(dropGapAnimation);
    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    dragGapIndex = QPersistentModelIndex();
}

void ChannelTree::resetDragVisuals(bool animate)
{
    if (animate) {
        clearDropGap(true);
        animateSourceCollapse(0.0);
        return;
    }

    stopAnimation(dropGapAnimation);
    stopAnimation(sourceCollapseAnimation);
    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
        }
    }
    dragGapIndex = QPersistentModelIndex();
    dragSourceIndexes.clear();
    draggedRowExtent = 0;
    doItemsLayout();
    viewport()->update();
}

QTreeWidgetItem* ChannelTree::channelDropGapAnchor(QTreeWidgetItem* source,
                                                   QTreeWidgetItem* targetCategoryItem,
                                                   const QString& targetChannelId,
                                                   bool afterTarget,
                                                   bool& gapAfter) const
{
    if (!targetCategoryItem) {
        return nullptr;
    }

    if (!targetChannelId.isEmpty()) {
        for (int i = 0; i < targetCategoryItem->childCount(); ++i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind
                && row->data(0, ItemIdRole).toString() == targetChannelId) {
                gapAfter = afterTarget;
                return row;
            }
        }
    }

    // Dropping on a category header means append inside that category. Put the
    // structural opening after its last visible child, not between the header
    // and its children.
    if (targetCategoryItem->isExpanded()) {
        for (int i = targetCategoryItem->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && row != source && !row->isHidden()) {
                gapAfter = true;
                return row;
            }
        }
    }
    gapAfter = true;
    return targetCategoryItem;
}

QTreeWidgetItem* ChannelTree::categoryDropGapAnchor(QTreeWidgetItem* targetCategoryItem,
                                                    bool afterTarget,
                                                    bool& gapAfter) const
{
    if (!targetCategoryItem) {
        return nullptr;
    }
    if (afterTarget && targetCategoryItem->isExpanded()) {
        for (int i = targetCategoryItem->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && !row->isHidden()) {
                gapAfter = true;
                return row;
            }
        }
    }
    gapAfter = afterTarget;
    return targetCategoryItem;
}

} // namespace Mattermost
''')

# ---------------------------------------------------------------------------
# Delegate geometry regression coverage.
# ---------------------------------------------------------------------------
replace_once(
    "tests/SidebarItemDelegateTest.cpp",
    "    void rendersPresenceForDirectMessage()\n",
    r'''    void dragGapParticipatesInRowGeometry()
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        model.appendRow(item);

        QListView view;
        view.setModel(&model);
        ChannelItemDelegate delegate;
        QStyleOptionViewItem option;
        option.initFrom(&view);

        const QModelIndex index = model.index(0, 0);
        QCOMPARE(delegate.sizeHint(option, index).height(), 32);

        item->setData(12, SidebarItem::DropGapBeforeRole);
        item->setData(8, SidebarItem::DropGapAfterRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 52);

        item->setData(0.5, SidebarItem::DragCollapseRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 36);
    }

    void rendersPresenceForDirectMessage()
''')

print("sidebar DnD UX patch applied")
