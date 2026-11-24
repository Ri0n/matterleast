/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QIcon>
#include <QPointer>
#include <QWidget>

#include "backend/emoji/EmojiDefs.h"

class QAbstractButton;
class QButtonGroup;
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
    explicit EmojiPickerWidget(QWidget* parent = nullptr);
    explicit EmojiPickerWidget(Backend& backend, QWidget* parent = nullptr);
    void setBackend(Backend* backend);

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
    QPushButton* createEmojiButton(const Emoji& emoji, QWidget* parent);
    void showSkinTonePopup(QPushButton* sourceButton,
                           const QString& baseName);
    void updateSkinToneButton(QPushButton* button,
                              const QString& baseName);
    void refreshDefaultSkinToneButtons();
    int defaultSkinTone() const;
    void setTabPresentation(int index,
                            const QString& glyph,
                            const QString& toolTip,
                            const QIcon& icon = QIcon());
    void removeTabPresentation(int index);
    void syncCurrentTabButton(int index);

    QPointer<Backend> backend_;
    QMetaObject::Connection customEmojiConnection_;
    QMetaObject::Connection backendDestroyedConnection_;
    QVector<Emoji> renderedCustomEmojis_;
    QLineEdit* searchEdit_ = nullptr;
    QWidget* categoryBar_ = nullptr;
    QStackedWidget* stackWidget_ = nullptr;
    QHBoxLayout* categoryBarLayout_ = nullptr;
    QButtonGroup* categoryButtonGroup_ = nullptr;
    QVector<QAbstractButton*> categoryButtons_;
    QVector<Emoji> searchableEmojis_;
    QWidget* searchTab_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QTimer* customEmojiRefreshTimer_ = nullptr;
    int searchReturnTabIndex_ = -1;
    int defaultSkinTone_ = EmojiSkinTone::none;
};

} // namespace Mattermost
