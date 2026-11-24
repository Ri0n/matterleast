#pragma once

#include <QTreeWidget>
#include <QSet>
#include <QMap>

namespace Mattermost {

// Keep the actual QTreeWidget rows for unchanged destinations. Callers hold
// the selection guard while applying a sorted projection, and set only roles
// belonging to that row. No model reset is needed on read/profile changes.
class SidebarQueueRows
{
public:
    static constexpr int KeyRole = Qt::UserRole + 100;

    SidebarQueueRows(QTreeWidget& tree, const QSet<QString>& desired)
        : tree_(tree)
    {
        for (int i = tree_.topLevelItemCount() - 1; i >= 0; --i) {
            auto* row = tree_.topLevelItem(i);
            const QString key = row->data(0, KeyRole).toString();
            if (!desired.contains(key))
                delete tree_.takeTopLevelItem(i);
            else
                rows_.insert(key, row);
        }
    }

    QTreeWidgetItem* place(const QString& key, int index)
    {
        auto* row = rows_.value(key);
        if (!row) {
            row = new QTreeWidgetItem;
            row->setData(0, KeyRole, key);
            tree_.insertTopLevelItem(index, row);
        } else {
            const int previous = tree_.indexOfTopLevelItem(row);
            if (previous != index) {
                const bool hidden = row->isHidden();
                tree_.takeTopLevelItem(previous);
                tree_.insertTopLevelItem(index, row);
                row->setHidden(hidden);
            }
        }
        return row;
    }

private:
    QTreeWidget& tree_;
    QMap<QString, QTreeWidgetItem*> rows_;
};

} // namespace Mattermost
