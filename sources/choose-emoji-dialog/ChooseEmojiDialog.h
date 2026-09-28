
/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#pragma once

#include <QDialog>
#include "backend/emoji/EmojiDefs.h"

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QIcon;
class QLayout;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace Ui {
class ChooseEmojiDialog;
}

namespace Mattermost {

class Backend;

class ChooseEmojiDialog: public QDialog {
private:
    explicit ChooseEmojiDialog (Backend& backend, QWidget *parent = nullptr);
    ~ChooseEmojiDialog();
public:
    void show ();
private:
    void createEmojiTabs ();
    void rebuildSearchableEmojis ();
    void refreshCustomEmojiCatalog ();
    void createTabForCategory (uint32_t categoryIndex, uint32_t tabIndex, const QString& tabName, const QVector<Emoji>& emojis);
    QLayout* createTab (uint32_t categoryIdx, int tabIndex);
    void updateSearchResults (const QString& text);
    void removeSearchTab ();
    Emoji getSelectedEmoji ();
    void addSkinToneComboBox (QWidget *tab, QVBoxLayout *layout, uint32_t categoryIdx);
    void setTabPresentation(int index,
                            const QString& glyph,
                            const QString& toolTip,
                            const QIcon& icon = QIcon());
    void removeTabPresentation(int index);
    void syncCurrentTabButton(int index);

private:
    friend class ChooseEmojiDialogWrapper;
    Backend&                 backend;
    Ui::ChooseEmojiDialog*	ui;
    QComboBox*				skinToneComboBox;
    QVector<QPushButton*>	peopleEmojiButtons;
    QVector<Emoji>          searchableEmojis;
    QWidget*                searchTab = nullptr;
    QTimer*                 searchTimer = nullptr;
    QTimer*                 customEmojiRefreshTimer = nullptr;
    QHBoxLayout*            categoryBarLayout = nullptr;
    QButtonGroup*           categoryButtonGroup = nullptr;
    QVector<QAbstractButton*> categoryButtons;
    int                     searchReturnTabIndex = -1;
    int                     renderedCustomEmojiCount = -1;
    Emoji					selectedEmoji;
};

} /* namespace Mattermost */
