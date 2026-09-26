#pragma once

#include <QSet>
#include <QStyledItemDelegate>

namespace Mattermost {

class ChannelItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChannelItemDelegate(QObject* parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

signals:
    void categoryActionRequested(const QString& teamId,
                                 const QString& categoryId);

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override;

private:
    mutable QSet<QString> requestedGroupChannels;
};

} // namespace Mattermost
