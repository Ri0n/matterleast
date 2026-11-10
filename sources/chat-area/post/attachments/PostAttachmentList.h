/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#ifndef MESSAGEATTACHMENTLIST_H
#define MESSAGEATTACHMENTLIST_H

#include <QWidget>
#include <list>

namespace Ui {
class PostAttachmentList;
}

class QEvent;
class QListWidgetItem;
class QToolButton;

namespace Mattermost {

class Backend;
class BackendFile;

class PostAttachmentList: public QWidget
{
    Q_OBJECT
public:
    explicit PostAttachmentList (Backend& backend, QWidget *parent = nullptr);
    ~PostAttachmentList();
public:
    // Determine the final mode before constructing any preview widgets.
    // At 4+ files show lightweight rows; the first three are visible.
    void setFiles(const std::list<BackendFile>& files, const QString& authorName);
    void updateDimensions ();

signals:
    void dimensionsChanged();

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshItemSizeHints();
    void setExpanded(bool expanded);
    void updateExpandLink();
    int _lastMeasuredViewportWidth = -1;
    bool _compact = false;
    bool _expanded = false;
    QToolButton* _expandButton = nullptr;
    Backend& 						backend;
    Ui::PostAttachmentList*			ui;
};

} /* namespace Mattermost */

#endif // MESSAGEATTACHMENTLIST_H


