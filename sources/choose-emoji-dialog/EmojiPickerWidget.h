/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QIcon>
#include <QPointer>
#include <QWidget>

#include "backend/emoji/EmojiDefs.h"

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLayout;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace Mattermost {

class Backend;

/**
 * Reusable emoji selector body used by both the standalone chooser dialog and
 * inline composer surfaces.
 */
class EmojiPickerWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit EmojiPickerWidget(Backend& backend, QWidget* parent = nullptr);

    void prepare();
    void resetSearch();
    void focusSearch();

signals:
    void emojiChosen(const Mattermost::Emoji& emoji);

private:
    void createEmojiTabs();
    void rebuildSearchableEmojis();
    void refreshCustomEmojiCatalog();
    void createTabForCategory(uint32_t categoryIndex,
                              uint32_t tabIndex,
                              const QString& tabName,
                              const QVector<Emoji>& emojis);
    QLayout* createTab(uint32_t categoryIdx, int tabIndex);
    void updateSearchResults(const QString& text);
    void removeSearchTab();
    void addSkinToneComboBox(QWidget* tab,
                             QVBoxLayout* layout,
                             uint32_t categoryIdx);
    void setTabPresentation(int index,
                            const QString& glyph,
                            const QString& toolTip,
                            const QIcon& icon = QIcon());
    void removeTabPresentation(int index);
    void syncCurrentTabButton(int index);

    Backend& backend_;
    QLineEdit* searchEdit_ = nullptr;
    QWidget* categoryBar_ = nullptr;
    QStackedWidget* stackWidget_ = nullptr;
    QHBoxLayout* categoryBarLayout_ = nullptr;
    QButtonGroup* categoryButtonGroup_ = nullptr;
    QVector<QAbstractButton*> categoryButtons_;
    QComboBox* skinToneComboBox_ = nullptr;
    QVector<QPushButton*> peopleEmojiButtons_;
    QVector<Emoji> searchableEmojis_;
    QWidget* searchTab_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QTimer* customEmojiRefreshTimer_ = nullptr;
    int searchReturnTabIndex_ = -1;
    int renderedCustomEmojiCount_ = -1;
};

} // namespace Mattermost
