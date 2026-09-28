/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ChooseEmojiDialog.h"

#include <algorithm>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDebug>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetItem>

#include "EmojiDialogSupport.h"
#include "backend/CustomEmojiService.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "ui_ChooseEmojiDialog.h"

namespace Mattermost {
namespace {

constexpr int EmojiButtonExtent = 32;
constexpr int EmojiTabExtent = 30;
constexpr int EmojiTabGlyphPointSize = 15;
constexpr int MaxSearchResults = 180;
constexpr char EmojiNameProperty[] = "mattermostEmojiName";
constexpr char EmojiValueProperty[] = "mattermostEmojiValue";

class EmojiFlowLayout final : public QLayout
{
public:
    explicit EmojiFlowLayout(QWidget* parent = nullptr)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(0);
    }

    ~EmojiFlowLayout() override
    {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override
    {
        items.push_back(item);
    }

    int count() const override
    {
        return items.size();
    }

    QLayoutItem* itemAt(int index) const override
    {
        return index >= 0 && index < items.size() ? items.at(index) : nullptr;
    }

    QLayoutItem* takeAt(int index) override
    {
        if (index < 0 || index >= items.size()) {
            return nullptr;
        }
        return items.takeAt(index);
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
        for (QLayoutItem* item : items) {
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

        int x = effective.x();
        int y = effective.y();
        int lineHeight = 0;

        for (QLayoutItem* item : items) {
            const QSize hint = item->sizeHint();
            const int nextX = x + hint.width();
            if (x > effective.x()
                && nextX > effective.right() + 1) {
                x = effective.x();
                y += lineHeight;
                lineHeight = 0;
            }

            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }

            x += hint.width();
            lineHeight = std::max(lineHeight, hint.height());
        }

        return y + lineHeight - rect.y() + bottom;
    }

    QVector<QLayoutItem*> items;
};

class EmojiTabButton final : public QAbstractButton
{
public:
    explicit EmojiTabButton(QWidget* parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(EmojiTabExtent, EmojiTabExtent);
        setFont(EmojiDialogSupport::emojiButtonFont(font(),
                                                     EmojiTabGlyphPointSize));
    }

    void setTabIcon(const QIcon& icon)
    {
        tabIcon = icon;
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(EmojiTabExtent, EmojiTabExtent);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (isChecked() || underMouse()) {
            QColor background = palette().color(
                isChecked() ? QPalette::Highlight : QPalette::Button);
            background.setAlpha(isChecked() ? 62 : 38);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        }

        if (!tabIcon.isNull()) {
            const QSize iconSize(20, 20);
            const QRect iconRect(
                (width() - iconSize.width()) / 2,
                (height() - iconSize.height()) / 2,
                iconSize.width(),
                iconSize.height());
            tabIcon.paint(&painter, iconRect, Qt::AlignCenter,
                          isEnabled() ? QIcon::Normal : QIcon::Disabled);
            return;
        }

        painter.setPen(palette().color(QPalette::ButtonText));
        painter.setFont(font());
        painter.drawText(rect(), Qt::AlignCenter, text());
    }

    bool event(QEvent* event) override
    {
        const bool handled = QAbstractButton::event(event);
        if (event->type() == QEvent::Enter
            || event->type() == QEvent::Leave) {
            update();
        }
        return handled;
    }

private:
    QIcon tabIcon;
};

int tabIndexForCategory(uint32_t categoryIdx)
{
    int tabIndex = 0;
    for (uint32_t index = 0; index < categoryIdx; ++index) {
        if (index != EmojiCategory::component) {
            ++tabIndex;
        }
    }
    return tabIndex;
}

const uint32_t indexForCategoryTab[EmojiCategory::COUNT] = {
    0,   // smileys-emotion
    98,  // people-body
    0,   // component
    1,   // animals-nature
    2,   // food-drink
    0,   // travel-places
    30,  // activities
    1,   // objects
    120, // symbols
    0,   // flags
    0,   // custom
};

QString customEmojiImagePath(const QString& value)
{
    int first = value.indexOf(QLatin1Char('"'));
    if (first < 0) {
        return {};
    }
    ++first;
    const int second = value.indexOf(QLatin1Char('"'), first);
    if (second < 0) {
        return {};
    }

    QString path = value.mid(first, second - first);
    path.replace(QStringLiteral("qrc://"), QStringLiteral(":/"));
    return path;
}

} // namespace

ChooseEmojiDialog::ChooseEmojiDialog(Backend& backend, QWidget* parent)
    : QDialog(parent)
    , backend(backend)
    , ui(new Ui::ChooseEmojiDialog)
{
    ui->setupUi(this);

    categoryBarLayout = new QHBoxLayout(ui->categoryBar);
    categoryBarLayout->setContentsMargins(0, 0, 0, 0);
    categoryBarLayout->setSpacing(0);
    categoryBarLayout->addStretch(1);

    categoryButtonGroup = new QButtonGroup(this);
    categoryButtonGroup->setExclusive(true);

    connect(ui->stackWidget, &QStackedWidget::currentChanged,
            this, &ChooseEmojiDialog::syncCurrentTabButton);

    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(100);
    connect(searchTimer, &QTimer::timeout, this, [this] {
        const QString text = ui->searchEdit->text();
        updateSearchResults(text);
        const QString search =
            EmojiDialogSupport::customEmojiServerSearchTerm(text);
        if (!search.isEmpty()) {
            CustomEmojiService::instance(this->backend).searchEmojis(search);
        }
    });
    connect(ui->searchEdit, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        if (EmojiDialogSupport::normalizeSearchTerm(text).isEmpty()) {
            searchTimer->stop();
            removeSearchTab();
            return;
        }
        searchTimer->start();
    });

    connect(ui->stackWidget, &QStackedWidget::currentChanged,
            this, [this](int index) {
        if (index == tabIndexForCategory(EmojiCategory::custom)) {
            CustomEmojiService::instance(this->backend).ensureBrowsePageLoaded();
        }
    });

    customEmojiRefreshTimer = new QTimer(this);
    customEmojiRefreshTimer->setSingleShot(true);
    customEmojiRefreshTimer->setInterval(50);
    connect(customEmojiRefreshTimer, &QTimer::timeout, this, [this] {
        refreshCustomEmojiCatalog();
        if (!EmojiDialogSupport::normalizeSearchTerm(
                ui->searchEdit->text()).isEmpty()) {
            updateSearchResults(ui->searchEdit->text());
        }
    });
    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiAdded,
            this, [this](const QString&) {
        customEmojiRefreshTimer->start();
    });
}

ChooseEmojiDialog::~ChooseEmojiDialog()
{
    delete ui;
}

Emoji ChooseEmojiDialog::getSelectedEmoji()
{
    return selectedEmoji;
}

void ChooseEmojiDialog::show()
{
    createEmojiTabs();
    refreshCustomEmojiCatalog();
    if (ui->stackWidget->currentIndex()
        == tabIndexForCategory(EmojiCategory::custom)) {
        CustomEmojiService::instance(backend).ensureBrowsePageLoaded();
    }

    ui->searchEdit->clear();
    QDialog::show();
    ui->searchEdit->setFocus(Qt::ShortcutFocusReason);
}

QLayout* ChooseEmojiDialog::createTab(uint32_t categoryIdx, int tabIndex)
{
    QWidget* tab = new QWidget;
    tab->setObjectName(
        QStringLiteral("tab") + QString::number(categoryIdx));
    tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* pageLayout = new QVBoxLayout(tab);
    pageLayout->setSpacing(4);
    pageLayout->setContentsMargins(0, 8, 0, 0);

    if (categoryIdx == EmojiCategory::people) {
        addSkinToneComboBox(tab, pageLayout, categoryIdx);
    }

    auto* flowHost = new QWidget(tab);
    flowHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* flowLayout = new EmojiFlowLayout(flowHost);
    pageLayout->addWidget(flowHost, 0);
    pageLayout->addStretch(1);

    if (tabIndex >= ui->stackWidget->count()) {
        ui->stackWidget->addWidget(tab);
    } else {
        QWidget* previousTab = ui->stackWidget->widget(tabIndex);
        ui->stackWidget->removeWidget(previousTab);
        if (previousTab && previousTab != searchTab) {
            previousTab->deleteLater();
        }
        ui->stackWidget->insertWidget(tabIndex, tab);
    }

    return flowLayout;
}

void ChooseEmojiDialog::createEmojiTabs()
{
    if (ui->stackWidget->count() >= 1) {
        return;
    }

    uint32_t tabIndex = 0;
    for (uint32_t categoryIdx = 0;
         categoryIdx < EmojiCategory::COUNT;
         ++categoryIdx) {
        if (categoryIdx == EmojiCategory::component) {
            continue;
        }

        const QVector<Emoji> emojis =
            EmojiInfo::getAllEmojis(categoryIdx, 0);
        createTabForCategory(
            categoryIdx,
            tabIndex,
            categoryDisplayNames[categoryIdx],
            emojis);
        ++tabIndex;
    }

    rebuildSearchableEmojis();
    renderedCustomEmojiCount =
        EmojiInfo::getAllEmojis(EmojiCategory::custom, 0).size();

    if (ui->stackWidget->count() > 0
        && ui->stackWidget->currentIndex() < 0) {
        ui->stackWidget->setCurrentIndex(0);
    }
    syncCurrentTabButton(ui->stackWidget->currentIndex());
}

void ChooseEmojiDialog::rebuildSearchableEmojis()
{
    searchableEmojis.clear();
    for (uint32_t categoryIdx = 0;
         categoryIdx < EmojiCategory::COUNT;
         ++categoryIdx) {
        if (categoryIdx == EmojiCategory::component) {
            continue;
        }
        searchableEmojis += EmojiInfo::getAllEmojis(categoryIdx, 0);
    }
}

void ChooseEmojiDialog::refreshCustomEmojiCatalog()
{
    rebuildSearchableEmojis();

    const QVector<Emoji> customEmojis =
        EmojiInfo::getAllEmojis(EmojiCategory::custom, 0);
    if (ui->stackWidget->count() < 1
        || customEmojis.size() == renderedCustomEmojiCount) {
        return;
    }

    createTabForCategory(
        EmojiCategory::custom,
        tabIndexForCategory(EmojiCategory::custom),
        categoryDisplayNames[EmojiCategory::custom],
        customEmojis);
    renderedCustomEmojiCount = customEmojis.size();
}

void ChooseEmojiDialog::createTabForCategory(
    uint32_t categoryIndex,
    uint32_t tabIndex,
    const QString& tabName,
    const QVector<Emoji>& emojis)
{
    QLayout* flowLayout = createTab(categoryIndex, tabIndex);
    QIcon categoryIcon;

    if (categoryIndex == EmojiCategory::people) {
        peopleEmojiButtons.clear();
        peopleEmojiButtons.reserve(emojis.size());
    }

    for (const Emoji& emoji : emojis) {
        auto* pushButton = new QPushButton(flowLayout->parentWidget());
        pushButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        pushButton->setFixedSize(QSize(EmojiButtonExtent, EmojiButtonExtent));
        pushButton->setText(emoji.unicodeString);
        pushButton->setToolTip(emoji.name);
        pushButton->setFont(
            EmojiDialogSupport::emojiButtonFont(QFont()));
        pushButton->setFlat(true);
        pushButton->setProperty(EmojiNameProperty, emoji.name);
        pushButton->setProperty(EmojiValueProperty, emoji.unicodeString);

        const QString imagePath =
            customEmojiImagePath(emoji.unicodeString);
        if (!imagePath.isEmpty()) {
            const QIcon icon(
                QPixmap::fromImage(QImage(imagePath)));
            pushButton->setText(QString());
            pushButton->setIcon(icon);
            pushButton->setIconSize(QSize(24, 24));

            if (categoryIndex == EmojiCategory::custom
                && emoji.name == QStringLiteral("mattermost")) {
                categoryIcon = icon;
            }
        }

        connect(pushButton, &QPushButton::clicked,
                this, [this, pushButton] {
            selectedEmoji.name =
                pushButton->property(EmojiNameProperty).toString();
            selectedEmoji.unicodeString =
                pushButton->property(EmojiValueProperty).toString();
            accept();
        });

        flowLayout->addWidget(pushButton);
        if (categoryIndex == EmojiCategory::people) {
            peopleEmojiButtons.push_back(pushButton);
        }
    }

    QString glyph;
    if (categoryIndex != EmojiCategory::custom
        && !emojis.isEmpty()) {
        const uint32_t iconIndex =
            indexForCategoryTab[categoryIndex];
        if (iconIndex < static_cast<uint32_t>(emojis.size())) {
            glyph = emojis.at(static_cast<int>(iconIndex)).unicodeString;
        }
    }

    setTabPresentation(
        static_cast<int>(tabIndex),
        glyph,
        tabName,
        categoryIcon);
}

void ChooseEmojiDialog::setTabPresentation(
    int index,
    const QString& glyph,
    const QString& toolTip,
    const QIcon& icon)
{
    while (categoryButtons.size() <= index) {
        categoryButtons.push_back(nullptr);
    }

    auto* button =
        static_cast<EmojiTabButton*>(categoryButtons.at(index));
    if (!button) {
        button = new EmojiTabButton(ui->categoryBar);
        categoryButtons[index] = button;
        categoryButtonGroup->addButton(button, index);
        categoryBarLayout->insertWidget(index, button);
        connect(button, &QAbstractButton::clicked,
                this, [this, index] {
            if (index >= 0 && index < ui->stackWidget->count()) {
                ui->stackWidget->setCurrentIndex(index);
            }
        });
    }

    button->setText(glyph);
    button->setTabIcon(icon);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    button->setChecked(index == ui->stackWidget->currentIndex());
}

void ChooseEmojiDialog::removeTabPresentation(int index)
{
    if (index < 0 || index >= categoryButtons.size()) {
        return;
    }

    QAbstractButton* button = categoryButtons.at(index);
    if (button) {
        categoryButtonGroup->removeButton(button);
        delete button;
    }
    categoryButtons.removeAt(index);
}

void ChooseEmojiDialog::syncCurrentTabButton(int index)
{
    for (int i = 0; i < categoryButtons.size(); ++i) {
        if (QAbstractButton* button = categoryButtons.at(i)) {
            button->setChecked(i == index);
        }
    }
}

void ChooseEmojiDialog::updateSearchResults(const QString& text)
{
    const QString search =
        EmojiDialogSupport::normalizeSearchTerm(text);
    if (search.isEmpty()) {
        removeSearchTab();
        return;
    }

    if (searchReturnTabIndex < 0) {
        searchReturnTabIndex = ui->stackWidget->currentIndex();
    }

    if (searchTab) {
        const int oldIndex =
            ui->stackWidget->indexOf(searchTab);
        if (oldIndex >= 0) {
            ui->stackWidget->removeWidget(searchTab);
            removeTabPresentation(oldIndex);
        }
        delete searchTab;
        searchTab = nullptr;
    }

    QVector<Emoji> matches;
    matches.reserve(std::min(
        static_cast<int>(searchableEmojis.size()),
        MaxSearchResults));

    for (int pass = 0;
         pass < 2 && matches.size() < MaxSearchResults;
         ++pass) {
        for (const Emoji& emoji : searchableEmojis) {
            const QString name =
                EmojiDialogSupport::normalizeSearchTerm(emoji.name);
            const bool prefix = name.startsWith(search);
            const bool matchesTerm =
                EmojiDialogSupport::matchesSearch(name, search);
            if ((pass == 0) != prefix || !matchesTerm) {
                continue;
            }
            matches.push_back(emoji);
            if (matches.size() >= MaxSearchResults) {
                break;
            }
        }
    }

    searchTab = new QWidget;
    searchTab->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* pageLayout = new QVBoxLayout(searchTab);
    pageLayout->setContentsMargins(0, 8, 0, 0);
    pageLayout->setSpacing(4);

    if (matches.isEmpty()) {
        auto* emptyLabel =
            new QLabel(tr("No emoji found"), searchTab);
        emptyLabel->setAlignment(Qt::AlignCenter);
        pageLayout->addWidget(emptyLabel, 1);
    } else {
        auto* flowHost = new QWidget(searchTab);
        flowHost->setSizePolicy(
            QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* flowLayout = new EmojiFlowLayout(flowHost);

        for (const Emoji& emoji : matches) {
            auto* pushButton = new QPushButton(flowHost);
            pushButton->setSizePolicy(
                QSizePolicy::Fixed, QSizePolicy::Fixed);
            pushButton->setFixedSize(
                QSize(EmojiButtonExtent, EmojiButtonExtent));
            pushButton->setText(emoji.unicodeString);
            pushButton->setToolTip(emoji.name);
            pushButton->setFont(
                EmojiDialogSupport::emojiButtonFont(QFont()));
            pushButton->setFlat(true);

            const QString imagePath =
                customEmojiImagePath(emoji.unicodeString);
            if (!imagePath.isEmpty()) {
                pushButton->setText(QString());
                pushButton->setIcon(
                    QIcon(QPixmap::fromImage(QImage(imagePath))));
                pushButton->setIconSize(QSize(24, 24));
            }

            connect(pushButton, &QPushButton::clicked,
                    this, [this, emoji] {
                selectedEmoji = emoji;
                accept();
            });
            flowLayout->addWidget(pushButton);
        }

        pageLayout->addWidget(flowHost, 0);
        pageLayout->addStretch(1);
    }

    const int searchIndex =
        ui->stackWidget->addWidget(searchTab);
    setTabPresentation(
        searchIndex,
        QStringLiteral("\U0001F50D"),
        tr("Search"));
    ui->stackWidget->setCurrentIndex(searchIndex);
}

void ChooseEmojiDialog::removeSearchTab()
{
    if (!searchTab) {
        searchReturnTabIndex = -1;
        return;
    }

    const int searchIndex =
        ui->stackWidget->indexOf(searchTab);
    if (searchIndex >= 0) {
        ui->stackWidget->removeWidget(searchTab);
        removeTabPresentation(searchIndex);
    }
    delete searchTab;
    searchTab = nullptr;

    if (searchReturnTabIndex >= 0
        && ui->stackWidget->count() > 0) {
        ui->stackWidget->setCurrentIndex(
            qBound(0,
                   searchReturnTabIndex,
                   ui->stackWidget->count() - 1));
    }
    searchReturnTabIndex = -1;
}

void ChooseEmojiDialog::addSkinToneComboBox(
    QWidget* tab,
    QVBoxLayout* layout,
    uint32_t categoryIdx)
{
    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);

    auto* label = new QLabel(tr("Skin Tone:"), tab);
    controls->addWidget(label);

    skinToneComboBox = new QComboBox(tab);
    skinToneComboBox->setToolTip(
        tr("Emojis from this category have a skin tone property."));
    for (int i = 0; i < EmojiSkinTone::COUNT; ++i) {
        skinToneComboBox->addItem(
            EmojiSkinTone::descriptionString[i], i);
    }
    controls->addWidget(skinToneComboBox);
    controls->addStretch(1);
    layout->addLayout(controls);

    connect(
        skinToneComboBox,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, categoryIdx](int index) {
        qDebug() << "Set skin tone " << index;
        const QVector<Emoji> emojis =
            EmojiInfo::getAllEmojis(categoryIdx, index);

        for (int i = 0; i < emojis.size(); ++i) {
            if (i >= peopleEmojiButtons.size()) {
                qDebug() << "Emoji index " << i
                         << " exceeds peopleEmojiButtons count"
                         << peopleEmojiButtons.size();
                return;
            }

            QPushButton* button = peopleEmojiButtons.at(i);
            button->setToolTip(
                emojis.at(i).name
                + EmojiSkinTone::nameString[index]);
            button->setText(emojis.at(i).unicodeString);
            button->setProperty(
                EmojiNameProperty,
                emojis.at(i).name
                    + EmojiSkinTone::nameString[index]);
            button->setProperty(
                EmojiValueProperty,
                emojis.at(i).unicodeString);
        }
    });
}

} // namespace Mattermost
