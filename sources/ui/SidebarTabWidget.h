#pragma once

#include <QStyle>
#include <QTabBar>
#include <QTabWidget>

namespace Mattermost {

// Keep the native tab layout and painting, but allow text tabs to shrink
// down to an icon-sized target instead of scrolling the Servers tab away.
class SidebarTabBar final : public QTabBar
{
public:
    explicit SidebarTabBar(QWidget* parent = nullptr) : QTabBar(parent)
    {
        setUsesScrollButtons(false);
        setElideMode(Qt::ElideRight);
        setExpanding(false);
    }

protected:
    QSize minimumTabSizeHint(int index) const override
    {
        QSize size = QTabBar::tabSizeHint(index);
        if (!tabText(index).isEmpty()) {
            const int padding = style()->pixelMetric(QStyle::PM_TabBarTabHSpace, nullptr, this);
            size.setWidth(qMin(size.width(), iconSize().width() + qMax(8, padding)));
        }
        return size;
    }
};

class SidebarTabWidget final : public QTabWidget
{
public:
    explicit SidebarTabWidget(QWidget* parent = nullptr) : QTabWidget(parent)
    {
        setTabBar(new SidebarTabBar(this));
    }
};

} // namespace Mattermost
