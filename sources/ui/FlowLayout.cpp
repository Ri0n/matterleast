/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "FlowLayout.h"

#include <algorithm>

#include <QLayoutItem>
#include <QWidget>

namespace Mattermost {

FlowLayout::FlowLayout(QWidget* parent, int margin, int spacing)
    : QLayout(parent)
{
    setContentsMargins(margin, margin, margin, margin);
    setSpacing(spacing);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item)
{
    items_.push_back(item);
}

int FlowLayout::count() const
{
    return items_.size();
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= items_.size()) {
        return nullptr;
    }
    return items_.takeAt(index);
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize result;
    for (QLayoutItem* item : items_) {
        result = result.expandedTo(item->minimumSize());
    }

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    result += QSize(left + right, top + bottom);
    return result;
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(left, top, -right, -bottom);

    const int gap = std::max(0, spacing());
    int x = effective.x();
    int y = effective.y();
    int lineHeight = 0;

    for (QLayoutItem* item : items_) {
        const QSize hint = item->sizeHint();
        const int nextX = x + hint.width();
        if (x > effective.x() && nextX > effective.right() + 1) {
            x = effective.x();
            y += lineHeight + gap;
            lineHeight = 0;
        }

        if (!testOnly) {
            item->setGeometry(QRect(QPoint(x, y), hint));
        }

        x += hint.width() + gap;
        lineHeight = std::max(lineHeight, hint.height());
    }

    return y + lineHeight - rect.y() + bottom;
}

} // namespace Mattermost
