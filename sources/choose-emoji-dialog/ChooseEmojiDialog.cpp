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

#include "ChooseEmojiDialog.h"

#include <algorithm>

#include <QComboBox>
#include <QDebug>
#include <QGridLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVariant>

#include "EmojiDialogSupport.h"
#include "backend/CustomEmojiService.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "ui_ChooseEmojiDialog.h"

namespace Mattermost {

static constexpr int maxItemsPerRow = 30;
static constexpr int emojiButtonExtent = 32;
static constexpr int maxSearchResults = 180;
static constexpr char EmojiButtonProperty[] = "mattermostEmojiPickerButton";
static constexpr char EmojiOrderProperty[] = "mattermostEmojiPickerOrder";
static constexpr char EmojiFirstRowProperty[] = "mattermostEmojiPickerFirstRow";
static constexpr char EmojiStretchColumnProperty[] = "mattermostEmojiPickerStretchColumn";
static constexpr char EmojiStretchRowProperty[] = "mattermostEmojiPickerStretchRow";

static int tabIndexForCategory(uint32_t categoryIdx)
{
    int tabIndex = 0;
    for (uint32_t index = 0; index < categoryIdx; ++index) {
        if (index != EmojiCategory::component) {
            ++tabIndex;
        }
    }
    return tabIndex;
}

/**
 * Index of the emoji to be shown before the tab name for the current category.
 * The index is set to specify the emoji which most cleary describles the meaning of the category
 */
static const uint32_t indexForCategoryTab[EmojiCategory::COUNT] = {
	0, 	//smileys-emotion
	98, //people-body
	0, 	//component
	1, 	//animals-nature
	2, 	//food-drink
	0, 	//travel-places
	30, //activities
	1,	//objects
	120,//symbols
	0,	//flags
	0,	//custom
};

ChooseEmojiDialog::ChooseEmojiDialog(Backend& backend, QWidget *parent)
:QDialog(parent)
,backend(backend)
,ui(new Ui::ChooseEmojiDialog)
{
	ui->setupUi(this);
    EmojiDialogSupport::configureTabWidget(*ui->tabWidget);
	ui->tabWidget->tabBar()->setFont(
		EmojiDialogSupport::emojiButtonFont(ui->tabWidget->font()));
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
	connect(ui->searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		if (EmojiDialogSupport::normalizeSearchTerm(text).isEmpty()) {
			searchTimer->stop();
			removeSearchTab();
			return;
		}
		searchTimer->start();
	});

    connect(ui->tabWidget, &QTabWidget::currentChanged, this,
            [this](int index) {
        if (index == tabIndexForCategory(EmojiCategory::custom)) {
            CustomEmojiService::instance(this->backend).ensureBrowsePageLoaded();
        }
    });

    customEmojiRefreshTimer = new QTimer(this);
    customEmojiRefreshTimer->setSingleShot(true);
    customEmojiRefreshTimer->setInterval(50);
    connect(customEmojiRefreshTimer, &QTimer::timeout, this, [this] {
        refreshCustomEmojiCatalog();
        if (!EmojiDialogSupport::normalizeSearchTerm(ui->searchEdit->text()).isEmpty()) {
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

Emoji ChooseEmojiDialog::getSelectedEmoji ()
{
	return selectedEmoji;
}

void ChooseEmojiDialog::show ()
{
	createEmojiTabs ();
    refreshCustomEmojiCatalog();
    if (ui->tabWidget->currentIndex()
        == tabIndexForCategory(EmojiCategory::custom)) {
        CustomEmojiService::instance(this->backend).ensureBrowsePageLoaded();
    }
	ui->searchEdit->clear ();
	QDialog::show ();
    reflowEmojiPages();
	ui->searchEdit->setFocus (Qt::ShortcutFocusReason);
}

QGridLayout* ChooseEmojiDialog::createTab (uint32_t categoryIdx, int tabIndex)
{
	QWidget *tab = new QWidget ();
	tab->setObjectName(QString::fromUtf8("tab") + QString::number(categoryIdx));
    tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    tab->setProperty(EmojiFirstRowProperty,
                     categoryIdx == EmojiCategory::people ? 1 : 0);
	QGridLayout *gridLayout = new QGridLayout(tab);
	gridLayout->setSpacing(0);
	gridLayout->setContentsMargins(0, 8, 0, 0);

	if (tabIndex >= ui->tabWidget->count()) {
		ui->tabWidget->addTab(tab, QString());
	} else {
        QWidget* previousTab = ui->tabWidget->widget(tabIndex);
		ui->tabWidget->removeTab (tabIndex);
        if (previousTab && previousTab != searchTab) {
            previousTab->deleteLater();
        }
		ui->tabWidget->insertTab (tabIndex, tab, QString());
	}
	return gridLayout;
}


void ChooseEmojiDialog::createEmojiTabs ()
{
	//tabs already created
	if (ui->tabWidget->count() >= 1) {
		return;
	}

	uint32_t tabIndex = 0;

	for (uint32_t categoryIdx = 0; categoryIdx < EmojiCategory::COUNT; ++categoryIdx) {

		/**
		 * The component category contains only the skin tone images, it is not useful alone
		 */
		if (categoryIdx == EmojiCategory::component) {
			continue;
		}

		QVector<Emoji> emojis = EmojiInfo::getAllEmojis (categoryIdx, 0);
		createTabForCategory (categoryIdx, tabIndex, categoryDisplayNames[categoryIdx], emojis);
		++tabIndex;
	}

    rebuildSearchableEmojis();
    renderedCustomEmojiCount =
        EmojiInfo::getAllEmojis(EmojiCategory::custom, 0).size();
}

void ChooseEmojiDialog::rebuildSearchableEmojis()
{
    searchableEmojis.clear();
    for (uint32_t categoryIdx = 0; categoryIdx < EmojiCategory::COUNT; ++categoryIdx) {
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
    if (ui->tabWidget->count() < 1
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

void ChooseEmojiDialog::createTabForCategory (uint32_t categoryIndex, uint32_t tabIndex, const QString& tabName, const QVector<Emoji>& emojis)
{
	QSizePolicy sizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	sizePolicy.setHorizontalStretch(0);
	sizePolicy.setVerticalStretch(0);

	QFont font = EmojiDialogSupport::emojiButtonFont (QFont());

	QGridLayout *gridLayout = createTab (categoryIndex, tabIndex);
    QWidget* page = ui->tabWidget->widget(tabIndex);

	/**
	 * For the 'people' category, add a combobox for settings skin tone
	 */
	if (categoryIndex == EmojiCategory::people) {
		addSkinToneComboBox (page, gridLayout, categoryIndex);
		peopleEmojiButtons.reserve (emojis.size());
	}

    int emojiOrder = 0;
	for (auto& emoji: emojis) {
		QPushButton* pushButton = new QPushButton (page);
        pushButton->setProperty(EmojiButtonProperty, true);
        pushButton->setProperty(EmojiOrderProperty, emojiOrder++);

		pushButton->setSizePolicy(sizePolicy);
		pushButton->setMinimumSize(QSize(32, 32));
		pushButton->setMaximumSize(QSize(32, 32));
		pushButton->setText (emoji.unicodeString);
		pushButton->setToolTip (emoji.name);
		pushButton->setFont(font);
		pushButton->setFlat(true);

		/**
		 * Replace the unicode string with icon for custom emojis
		 * This is how they work, when on a button in the dialog.
		 */
		QString str (emoji.unicodeString);
		int found1 = str.indexOf ('"');

		if (found1 != -1) {
			++found1;
			int found2 = str.indexOf ('"', found1);
			if (found2 != -1) {
				QString path (str.mid (found1, found2-found1));
				path.replace("qrc://",":/");
				qDebug() << "Use path " << path;
				QIcon icon (QPixmap::fromImage(QImage(path)));
				pushButton->setText ("");
				pushButton->setIcon (icon);
				pushButton->setIconSize(QSize(24,24));

				//Use the 'mattermost' emoji for the 'custom' category's icon
				if (categoryIndex == EmojiCategory::custom && emoji.name == "mattermost") {
					ui->tabWidget->setTabIcon (tabIndex, icon);
				}
			}
		}

		connect (pushButton, &QPushButton::clicked, [this, pushButton] {
			selectedEmoji.name = pushButton->toolTip();
			selectedEmoji.unicodeString = pushButton->text();
			accept ();
		});

		gridLayout->addWidget(pushButton,
                              page->property(EmojiFirstRowProperty).toInt(),
                              emojiOrder - 1,
                              1,
                              1);

		if (categoryIndex == EmojiCategory::people) {
			peopleEmojiButtons.push_back(pushButton);
		}
	}

	/**
	 * Set tab text and icon
	 */
	QString iconString;

	if (categoryIndex != EmojiCategory::custom) {
		iconString = emojis[indexForCategoryTab[categoryIndex]].unicodeString;
	}
	ui->tabWidget->setTabText (tabIndex, iconString);
	ui->tabWidget->setTabToolTip (tabIndex, tabName);

    reflowEmojiPage(page);
}

void ChooseEmojiDialog::updateSearchResults (const QString& text)
{
	QString search = EmojiDialogSupport::normalizeSearchTerm (text);
	if (search.isEmpty()) {
		removeSearchTab ();
		return;
	}

	if (searchReturnTabIndex < 0) {
		searchReturnTabIndex = ui->tabWidget->currentIndex ();
	}
	if (searchTab) {
		int oldIndex = ui->tabWidget->indexOf (searchTab);
		if (oldIndex >= 0) {
			ui->tabWidget->removeTab (oldIndex);
		}
		delete searchTab;
		searchTab = nullptr;
	}

	QVector<Emoji> matches;
	matches.reserve (std::min (static_cast<int>(searchableEmojis.size()), maxSearchResults));

	// Prefix matches are more useful for short queries. Fill them first, then
	// append ordered-token matches without rebuilding any of the category tabs.
	for (int pass = 0; pass < 2 && matches.size() < maxSearchResults; ++pass) {
		for (const Emoji& emoji: searchableEmojis) {
			QString name = EmojiDialogSupport::normalizeSearchTerm (emoji.name);
			bool prefix = name.startsWith (search);
			bool matchesTerm = EmojiDialogSupport::matchesSearch (name, search);
			if ((pass == 0) != prefix || !matchesTerm) {
				continue;
			}
			matches.push_back (emoji);
			if (matches.size() >= maxSearchResults) {
				break;
			}
		}
	}

	searchTab = new QWidget ();
    searchTab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    searchTab->setProperty(EmojiFirstRowProperty, 0);
	QGridLayout* gridLayout = new QGridLayout (searchTab);
	gridLayout->setSpacing (0);
	gridLayout->setContentsMargins (0, 8, 0, 0);
	QFont font = EmojiDialogSupport::emojiButtonFont (QFont());

	int emojiOrder = 0;
	for (const Emoji& emoji: matches) {
		QPushButton* pushButton = new QPushButton (searchTab);
        pushButton->setProperty(EmojiButtonProperty, true);
        pushButton->setProperty(EmojiOrderProperty, emojiOrder++);
		pushButton->setSizePolicy (QSizePolicy::Fixed, QSizePolicy::Fixed);
		pushButton->setMinimumSize (QSize(32, 32));
		pushButton->setMaximumSize (QSize(32, 32));
		pushButton->setText (emoji.unicodeString);
		pushButton->setToolTip (emoji.name);
		pushButton->setFont (font);
		pushButton->setFlat (true);

		QString str (emoji.unicodeString);
		int found1 = str.indexOf ('"');
		if (found1 != -1) {
			++found1;
			int found2 = str.indexOf ('"', found1);
			if (found2 != -1) {
				QString path (str.mid (found1, found2-found1));
				path.replace ("qrc://", ":/");
				pushButton->setText ("");
				pushButton->setIcon (QIcon(QPixmap::fromImage(QImage(path))));
				pushButton->setIconSize (QSize(24,24));
			}
		}

		connect (pushButton, &QPushButton::clicked, [this, emoji] {
			selectedEmoji = emoji;
			accept ();
		});
		gridLayout->addWidget (pushButton, 0, emojiOrder - 1, 1, 1);
	}

	if (matches.isEmpty()) {
		QLabel* emptyLabel = new QLabel (tr("No emoji found"), searchTab);
		gridLayout->addWidget (emptyLabel, 0, 0, 1, 1, Qt::AlignCenter);
	}

	int searchIndex = ui->tabWidget->addTab (searchTab,
											QStringLiteral("\U0001F50D"));
	ui->tabWidget->setTabToolTip (searchIndex, tr("Search"));
	ui->tabWidget->setCurrentIndex (searchIndex);
    reflowEmojiPage(searchTab);
}

void ChooseEmojiDialog::removeSearchTab ()
{
	if (!searchTab) {
		searchReturnTabIndex = -1;
		return;
	}

	int searchIndex = ui->tabWidget->indexOf (searchTab);
	if (searchIndex >= 0) {
		ui->tabWidget->removeTab (searchIndex);
	}
	delete searchTab;
	searchTab = nullptr;

	if (searchReturnTabIndex >= 0 && ui->tabWidget->count() > 0) {
		ui->tabWidget->setCurrentIndex (qBound(0, searchReturnTabIndex, ui->tabWidget->count() - 1));
	}
	searchReturnTabIndex = -1;
}

void ChooseEmojiDialog::reflowEmojiPage(QWidget* page)
{
    if (!page) {
        return;
    }

    auto* gridLayout = qobject_cast<QGridLayout*>(page->layout());
    if (!gridLayout) {
        return;
    }

    QVector<QPushButton*> buttons;
    const auto children =
        page->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly);
    for (QPushButton* button : children) {
        if (button->property(EmojiButtonProperty).toBool()) {
            buttons.push_back(button);
        }
    }
    std::sort(buttons.begin(), buttons.end(),
              [](const QPushButton* lhs, const QPushButton* rhs) {
        return lhs->property(EmojiOrderProperty).toInt()
            < rhs->property(EmojiOrderProperty).toInt();
    });

    const QVariant previousStretchColumn =
        page->property(EmojiStretchColumnProperty);
    if (previousStretchColumn.isValid()) {
        gridLayout->setColumnStretch(previousStretchColumn.toInt(), 0);
    }
    const QVariant previousStretchRow = page->property(EmojiStretchRowProperty);
    if (previousStretchRow.isValid()) {
        gridLayout->setRowStretch(previousStretchRow.toInt(), 0);
    }

    const QMargins margins = gridLayout->contentsMargins();
    const int availableWidth = std::max(
        emojiButtonExtent,
        ui->tabWidget->contentsRect().width()
            - margins.left() - margins.right());
    const int columns = std::max(
        1, std::min(maxItemsPerRow, availableWidth / emojiButtonExtent));
    const int firstRow = page->property(EmojiFirstRowProperty).toInt();

    for (QPushButton* button : buttons) {
        gridLayout->removeWidget(button);
    }
    for (int index = 0; index < buttons.size(); ++index) {
        gridLayout->addWidget(buttons.at(index),
                              firstRow + index / columns,
                              index % columns,
                              1,
                              1,
                              Qt::AlignLeft | Qt::AlignTop);
    }

    const int stretchColumn = buttons.isEmpty()
        ? 0 : std::max(columns, firstRow > 0 ? 7 : 0);
    gridLayout->setColumnStretch(stretchColumn, 1);
    page->setProperty(EmojiStretchColumnProperty, stretchColumn);

    const int rows = buttons.isEmpty()
        ? 1 : (buttons.size() + columns - 1) / columns;
    const int stretchRow = firstRow + rows;
    gridLayout->setRowStretch(stretchRow, 1);
    page->setProperty(EmojiStretchRowProperty, stretchRow);

}

void ChooseEmojiDialog::reflowEmojiPages()
{
    for (int index = 0; index < ui->tabWidget->count(); ++index) {
        reflowEmojiPage(ui->tabWidget->widget(index));
    }
}

void ChooseEmojiDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    reflowEmojiPages();
}

void ChooseEmojiDialog::addSkinToneComboBox (QWidget *tab, QGridLayout *gridLayout, uint32_t categoryIdx)
{
	QLabel *label = new QLabel(tab);
	label->setText ("Skin Tone:");
	gridLayout->addWidget(label, 0, 0, 1, 2);

	skinToneComboBox = new QComboBox(tab);
	skinToneComboBox->setToolTip ("Emojis from this category have a 'Skin Tone' property,\nwhich can modify the skin color, making it different from the classic one (yellow)");

	for (int i = 0; i < EmojiSkinTone::COUNT; ++i) {
		skinToneComboBox->addItem (EmojiSkinTone::descriptionString[i], i);
	}

	connect (skinToneComboBox, qOverload<int>(&QComboBox::currentIndexChanged), [this, categoryIdx] (int index) {
		qDebug () << "Set skin tone " << index;
		QVector<Emoji> emojis = EmojiInfo::getAllEmojis (categoryIdx, index);

		for (int i = 0; i < emojis.size(); ++i) {
			if (i >= peopleEmojiButtons.size()) {
				qDebug() << "Emoji index " << i << " exceeds peopleEmojiButtons count" << peopleEmojiButtons.size();
				return;
			}

			peopleEmojiButtons[i]->setToolTip (emojis[i].name + EmojiSkinTone::nameString[index]);
			peopleEmojiButtons[i]->setText (emojis[i].unicodeString);
		}
	});

	gridLayout->addWidget (skinToneComboBox, 0, 3, 1, 4);
}

} /* namespace Mattermost */
