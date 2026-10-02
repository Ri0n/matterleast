/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <algorithm>

#include <QLayout>
#include <QLayoutItem>
#include <QVector>
#include <QWidget>

namespace Mattermost {

/**
 * Simple height-for-width flow layout. Child items keep their size hints and
 * wrap to the next row when the available width is exhausted.
 *
 * The implementation is intentionally header-only: several lightweight widget
 * tests compile their UI helpers directly instead of linking matterleast-core,
 * and using FlowLayout there should not require mirroring a production source
 * list merely to obtain this small layout primitive.
 */
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr,
                        int margin = 0,
                        int spacing = 0)
        : QLayout(parent)
    {
        setContentsMargins(margin, margin, margin, margin);
        setSpacing(spacing);
    }

    ~FlowLayout() override
    {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override
    {
        items_.push_back(item);
    }

    int count() const override
    {
        return items_.size();
    }

    QLayoutItem* itemAt(int index) const override
    {
        return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
    }

    QLayoutItem* takeAt(int index) override
    {
        if (index < 0 || index >= items_.size()) {
            return nullptr;
        }
        return items_.takeAt(index);
    }

    Qt::Orientations expandingDirections() const override
    {
        return {};
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    QSize sizeHint() const override
    {
        return minimumSize();
    }

    QSize minimumSize() const override
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

    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const
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

    QVector<QLayoutItem*> items_;
};

} // namespace Mattermost
