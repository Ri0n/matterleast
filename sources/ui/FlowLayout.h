/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QLayout>
#include <QVector>

namespace Mattermost {

/**
 * Simple height-for-width flow layout. Child items keep their size hints and
 * wrap to the next row when the available width is exhausted.
 */
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr,
                        int margin = 0,
                        int spacing = 0);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;

    QVector<QLayoutItem*> items_;
};

} // namespace Mattermost
