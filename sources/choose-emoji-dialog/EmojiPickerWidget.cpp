#include "EmojiPickerWidget.h"

#include <algorithm>
#include <functional>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "EmojiDialogSupport.h"
#include "Settings.h"
#include "backend/Backend.h"
#include "backend/CustomEmojiService.h"
#include "backend/emoji/EmojiInfo.h"
#include "options/MLOptions.h"
#include "ui/FlowLayout.h"
#include "ui/OverlayScrollBarManager.h"

namespace Mattermost {
namespace {

constexpr int EmojiButtonExtent = 32;
constexpr int EmojiTabExtent = 30;
constexpr int EmojiTabGlyphPointSize = 15;
constexpr int MaxSearchResults = 180;
constexpr char EmojiNameProperty[] = "mattermostEmojiName";
constexpr char EmojiValueProperty[] = "mattermostEmojiValue";
constexpr char EmojiBaseNameProperty[] = "mattermostEmojiBaseName";

class EmojiFlowHost final : public QWidget
{
public:
    explicit EmojiFlowHost(QWidget* parent = nullptr)
        : QWidget(parent)
        , flowLayout_(new FlowLayout(this))
    {
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    FlowLayout* emojiLayout() const
    {
        return flowLayout_;
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        const int requiredHeight =
            flowLayout_->heightForWidth(std::max(1, width()));
        if (minimumHeight() != requiredHeight) {
            setMinimumHeight(requiredHeight);
            updateGeometry();
        }
    }

private:
    FlowLayout* flowLayout_ = nullptr;
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
        setFont(EmojiDialogSupport::emojiButtonFont(
            font(), EmojiTabGlyphPointSize));
    }

    void setTabIcon(const QIcon& icon)
    {
        tabIcon_ = icon;
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

        if (!tabIcon_.isNull()) {
            const QSize iconSize(20, 20);
            const QRect iconRect(
                (width() - iconSize.width()) / 2,
                (height() - iconSize.height()) / 2,
                iconSize.width(),
                iconSize.height());
            tabIcon_.paint(&painter, iconRect, Qt::AlignCenter,
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
    QIcon tabIcon_;
};

class EmojiButton final : public QPushButton
{
public:
    explicit EmojiButton(QWidget* parent = nullptr)
        : QPushButton(parent)
    {
        _longPressTimer.setSingleShot(true);
        _longPressTimer.setInterval(QApplication::startDragTime());
        QObject::connect(&_longPressTimer, &QTimer::timeout, this, [this] {
            if (!_longPressHandler) {
                return;
            }
            _longPressTriggered = true;
            setDown(false);
            releaseMouse();
            _longPressHandler();
        });
    }

    void setLongPressHandler(std::function<void()> handler)
    {
        _longPressHandler = std::move(handler);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        _longPressTriggered = false;
        if (event && event->button() == Qt::LeftButton && _longPressHandler) {
            _pressPosition = event->pos();
            _longPressTimer.start();
        }
        QPushButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (_longPressTimer.isActive() && event
            && (event->pos() - _pressPosition).manhattanLength()
                > QApplication::startDragDistance()) {
            _longPressTimer.stop();
        }
        QPushButton::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        _longPressTimer.stop();
        if (_longPressTriggered && event
            && event->button() == Qt::LeftButton) {
            _longPressTriggered = false;
            setDown(false);
            event->accept();
            return;
        }
        QPushButton::mouseReleaseEvent(event);
    }

private:
    QTimer _longPressTimer;
    QPoint _pressPosition;
    bool _longPressTriggered = false;
    std::function<void()> _longPressHandler;
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
    0,
    98,
    0,
    1,
    2,
    0,
    30,
    1,
    120,
    0,
    0,
};

QScrollArea* createEmojiScrollArea(QWidget* parent)
{
    auto* area = new QScrollArea(parent);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setBackgroundRole(QPalette::Base);
    area->setAutoFillBackground(true);
    area->viewport()->setBackgroundRole(QPalette::Base);
    area->viewport()->setAutoFillBackground(true);
    OverlayScrollBarManager::setEdgeNavigationButtonsEnabled(*area, false);
    return area;
}

void useBaseBackground(QWidget& widget)
{
    widget.setBackgroundRole(QPalette::Base);
    widget.setAutoFillBackground(true);
}

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

EmojiPickerWidget::EmojiPickerWidget(Backend& backend, QWidget* parent)
    : QWidget(parent)
    , backend_(backend)
{
    useBaseBackground(*this);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setSpacing(4);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setPlaceholderText(tr("Search emoji…"));
    searchEdit_->setAccessibleName(tr("Search emoji"));
    rootLayout->addWidget(searchEdit_);

    categoryBar_ = new QWidget(this);
    categoryBar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rootLayout->addWidget(categoryBar_);

    categoryBarLayout_ = new QHBoxLayout(categoryBar_);
    categoryBarLayout_->setContentsMargins(0, 0, 0, 0);
    categoryBarLayout_->setSpacing(0);
    categoryBarLayout_->addStretch(1);

    categoryButtonGroup_ = new QButtonGroup(this);
    categoryButtonGroup_->setExclusive(true);

    stackWidget_ = new QStackedWidget(this);
    stackWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rootLayout->addWidget(stackWidget_, 1);

    connect(stackWidget_, &QStackedWidget::currentChanged,
            this, &EmojiPickerWidget::syncCurrentTabButton);
    connect(stackWidget_, &QStackedWidget::currentChanged,
            this, [this](int index) {
        if (index == tabIndexForCategory(EmojiCategory::custom)) {
            CustomEmojiService::instance(backend_).ensureBrowsePageLoaded();
        }
    });

    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(100);
    connect(searchTimer_, &QTimer::timeout, this, [this] {
        const QString text = searchEdit_->text();
        updateSearchResults(text);
        const QString search =
            EmojiDialogSupport::customEmojiServerSearchTerm(text);
        if (!search.isEmpty()) {
            CustomEmojiService::instance(backend_).searchEmojis(search);
        }
    });
    connect(searchEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        if (EmojiDialogSupport::normalizeSearchTerm(text).isEmpty()) {
            searchTimer_->stop();
            removeSearchTab();
            return;
        }
        searchTimer_->start();
    });

    customEmojiRefreshTimer_ = new QTimer(this);
    customEmojiRefreshTimer_->setSingleShot(true);
    customEmojiRefreshTimer_->setInterval(50);
    connect(customEmojiRefreshTimer_, &QTimer::timeout, this, [this] {
        refreshCustomEmojiCatalog();
        if (!EmojiDialogSupport::normalizeSearchTerm(
                searchEdit_->text()).isEmpty()) {
            updateSearchResults(searchEdit_->text());
        }
    });
    connect(&backend_.emojiRegistry(),
            &EmojiRegistry::customEmojiAdded,
            this, [this](const QString&) {
        customEmojiRefreshTimer_->start();
    });
}

void EmojiPickerWidget::prepare()
{
    createEmojiTabs();
    refreshCustomEmojiCatalog();
    if (stackWidget_->currentIndex()
        == tabIndexForCategory(EmojiCategory::custom)) {
        CustomEmojiService::instance(backend_).ensureBrowsePageLoaded();
    }
    resetSearch();
}

void EmojiPickerWidget::resetSearch()
{
    if (searchTimer_) {
        searchTimer_->stop();
    }
    searchEdit_->clear();
    removeSearchTab();
}

void EmojiPickerWidget::focusSearch()
{
    searchEdit_->setFocus(Qt::ShortcutFocusReason);
}

QLayout* EmojiPickerWidget::createTab(uint32_t categoryIdx, int tabIndex)
{
    auto* tab = new QWidget;
    tab->setObjectName(
        QStringLiteral("tab") + QString::number(categoryIdx));
    tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    useBaseBackground(*tab);

    auto* pageLayout = new QVBoxLayout(tab);
    pageLayout->setSpacing(0);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    QScrollArea* scrollArea = createEmojiScrollArea(tab);
    auto* content = new QWidget;
    useBaseBackground(*content);

    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setSpacing(4);
    contentLayout->setContentsMargins(0, 8, 0, 0);

    auto* flowHost = new EmojiFlowHost(content);
    auto* flowLayout = flowHost->emojiLayout();
    contentLayout->addWidget(flowHost, 0);
    contentLayout->addStretch(1);

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea);

    if (tabIndex >= stackWidget_->count()) {
        stackWidget_->addWidget(tab);
    } else {
        QWidget* previousTab = stackWidget_->widget(tabIndex);
        stackWidget_->removeWidget(previousTab);
        if (previousTab && previousTab != searchTab_) {
            previousTab->deleteLater();
        }
        stackWidget_->insertWidget(tabIndex, tab);
    }

    return flowLayout;
}

void EmojiPickerWidget::createEmojiTabs()
{
    if (stackWidget_->count() >= 1) {
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
            backend_.emojiRegistry().getAllEmojis(categoryIdx, 0);
        createTabForCategory(
            categoryIdx,
            tabIndex,
            categoryDisplayNames[categoryIdx],
            emojis);
        ++tabIndex;
    }

    rebuildSearchableEmojis();
    renderedCustomEmojiCount_ =
        backend_.emojiRegistry().getAllEmojis(EmojiCategory::custom, 0).size();

    if (stackWidget_->count() > 0
        && stackWidget_->currentIndex() < 0) {
        stackWidget_->setCurrentIndex(0);
    }
    syncCurrentTabButton(stackWidget_->currentIndex());
}

void EmojiPickerWidget::rebuildSearchableEmojis()
{
    searchableEmojis_.clear();
    for (uint32_t categoryIdx = 0;
         categoryIdx < EmojiCategory::COUNT;
         ++categoryIdx) {
        if (categoryIdx == EmojiCategory::component) {
            continue;
        }
        searchableEmojis_ += backend_.emojiRegistry().getAllEmojis(categoryIdx, 0);
    }
}

void EmojiPickerWidget::refreshCustomEmojiCatalog()
{
    rebuildSearchableEmojis();

    const QVector<Emoji> customEmojis =
        backend_.emojiRegistry().getAllEmojis(EmojiCategory::custom, 0);
    if (stackWidget_->count() < 1
        || customEmojis.size() == renderedCustomEmojiCount_) {
        return;
    }

    createTabForCategory(
        EmojiCategory::custom,
        tabIndexForCategory(EmojiCategory::custom),
        categoryDisplayNames[EmojiCategory::custom],
        customEmojis);
    renderedCustomEmojiCount_ = customEmojis.size();
}

void EmojiPickerWidget::createTabForCategory(
    uint32_t categoryIndex,
    uint32_t tabIndex,
    const QString& tabName,
    const QVector<Emoji>& emojis)
{
    QLayout* flowLayout = createTab(categoryIndex, tabIndex);
    QIcon categoryIcon;

    for (const Emoji& emoji : emojis) {
        QPushButton* pushButton = createEmojiButton(
            emoji, flowLayout->parentWidget());
        flowLayout->addWidget(pushButton);

        if (categoryIndex == EmojiCategory::custom
            && emoji.name == QStringLiteral("mattermost")) {
            categoryIcon = pushButton->icon();
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

void EmojiPickerWidget::setTabPresentation(
    int index,
    const QString& glyph,
    const QString& toolTip,
    const QIcon& icon)
{
    while (categoryButtons_.size() <= index) {
        categoryButtons_.push_back(nullptr);
    }

    auto* button =
        static_cast<EmojiTabButton*>(categoryButtons_.at(index));
    if (!button) {
        button = new EmojiTabButton(categoryBar_);
        categoryButtons_[index] = button;
        categoryButtonGroup_->addButton(button, index);
        categoryBarLayout_->insertWidget(index, button);
        connect(button, &QAbstractButton::clicked,
                this, [this, index] {
            if (index >= 0 && index < stackWidget_->count()) {
                stackWidget_->setCurrentIndex(index);
            }
        });
    }

    button->setText(glyph);
    button->setTabIcon(icon);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    button->setChecked(index == stackWidget_->currentIndex());
}

void EmojiPickerWidget::removeTabPresentation(int index)
{
    if (index < 0 || index >= categoryButtons_.size()) {
        return;
    }

    QAbstractButton* button = categoryButtons_.at(index);
    if (button) {
        categoryButtonGroup_->removeButton(button);
        delete button;
    }
    categoryButtons_.removeAt(index);
}

void EmojiPickerWidget::syncCurrentTabButton(int index)
{
    for (int i = 0; i < categoryButtons_.size(); ++i) {
        if (QAbstractButton* button = categoryButtons_.at(i)) {
            button->setChecked(i == index);
        }
    }
}

void EmojiPickerWidget::updateSearchResults(const QString& text)
{
    const QString search =
        EmojiDialogSupport::normalizeSearchTerm(text);
    if (search.isEmpty()) {
        removeSearchTab();
        return;
    }

    if (searchReturnTabIndex_ < 0) {
        searchReturnTabIndex_ = stackWidget_->currentIndex();
    }

    if (searchTab_) {
        const int oldIndex = stackWidget_->indexOf(searchTab_);
        if (oldIndex >= 0) {
            stackWidget_->removeWidget(searchTab_);
            removeTabPresentation(oldIndex);
        }
        delete searchTab_;
        searchTab_ = nullptr;
    }

    QVector<Emoji> matches;
    matches.reserve(std::min(
        static_cast<int>(searchableEmojis_.size()),
        MaxSearchResults));

    for (int pass = 0;
         pass < 2 && matches.size() < MaxSearchResults;
         ++pass) {
        for (const Emoji& emoji : searchableEmojis_) {
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

    searchTab_ = new QWidget;
    searchTab_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    useBaseBackground(*searchTab_);

    auto* pageLayout = new QVBoxLayout(searchTab_);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    if (matches.isEmpty()) {
        auto* emptyLabel =
            new QLabel(tr("No emoji found"), searchTab_);
        emptyLabel->setAlignment(Qt::AlignCenter);
        pageLayout->addWidget(emptyLabel, 1);
    } else {
        QScrollArea* scrollArea = createEmojiScrollArea(searchTab_);
        auto* content = new QWidget;
        useBaseBackground(*content);

        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 8, 0, 0);
        contentLayout->setSpacing(0);

        auto* flowHost = new EmojiFlowHost(content);
        auto* flowLayout = flowHost->emojiLayout();

        for (const Emoji& emoji : matches) {
            flowLayout->addWidget(createEmojiButton(emoji, flowHost));
        }

        contentLayout->addWidget(flowHost, 0);
        contentLayout->addStretch(1);
        scrollArea->setWidget(content);
        pageLayout->addWidget(scrollArea);
    }

    const int searchIndex = stackWidget_->addWidget(searchTab_);
    setTabPresentation(
        searchIndex,
        QStringLiteral("\U0001F50D"),
        tr("Search"));
    stackWidget_->setCurrentIndex(searchIndex);
}

void EmojiPickerWidget::removeSearchTab()
{
    if (!searchTab_) {
        searchReturnTabIndex_ = -1;
        return;
    }

    const int searchIndex = stackWidget_->indexOf(searchTab_);
    if (searchIndex >= 0) {
        stackWidget_->removeWidget(searchTab_);
        removeTabPresentation(searchIndex);
    }
    delete searchTab_;
    searchTab_ = nullptr;

    if (searchReturnTabIndex_ >= 0
        && stackWidget_->count() > 0) {
        stackWidget_->setCurrentIndex(
            qBound(0,
                   searchReturnTabIndex_,
                   stackWidget_->count() - 1));
    }
    searchReturnTabIndex_ = -1;
}

int EmojiPickerWidget::defaultSkinTone() const
{
    return qBound(
        0,
        MLOptions::instance()->value<int>(
            EMOJI_DEFAULT_SKIN_TONE,
            EMOJI_DEFAULT_SKIN_TONE_DEFAULT),
        static_cast<int>(EmojiSkinTone::COUNT) - 1);
}

QPushButton* EmojiPickerWidget::createEmojiButton(
    const Emoji& emoji, QWidget* parent)
{
    auto* button = new EmojiButton(parent);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    button->setFixedSize(QSize(EmojiButtonExtent, EmojiButtonExtent));
    button->setFont(EmojiDialogSupport::emojiButtonFont(QFont()));
    button->setFlat(true);

    const QVector<Emoji> variants =
        EmojiInfo::skinToneVariantsByName(emoji.name);
    Emoji selected = emoji;
    if (!variants.isEmpty()) {
        selected = variants.at(qMin(defaultSkinTone(), variants.size() - 1));
        button->setProperty(EmojiBaseNameProperty, emoji.name);
        button->setLongPressHandler([this, button, baseName = emoji.name] {
            showSkinTonePopup(button, baseName);
        });
    }

    button->setText(selected.unicodeString);
    button->setToolTip(
        variants.isEmpty()
            ? selected.name
            : selected.name + QLatin1Char('\n')
                + tr("Hold to choose skin tone"));
    button->setProperty(EmojiNameProperty, selected.name);
    button->setProperty(EmojiValueProperty, selected.unicodeString);

    const QString imagePath = customEmojiImagePath(selected.unicodeString);
    if (!imagePath.isEmpty()) {
        button->setText(QString());
        button->setIcon(QIcon(QPixmap::fromImage(QImage(imagePath))));
        button->setIconSize(QSize(24, 24));
    }

    connect(button, &QPushButton::clicked, this, [this, button] {
        Emoji chosen;
        chosen.name = button->property(EmojiNameProperty).toString();
        chosen.unicodeString =
            button->property(EmojiValueProperty).toString();
        emit emojiChosen(chosen);
    });
    return button;
}

void EmojiPickerWidget::showSkinTonePopup(
    QPushButton* sourceButton, const QString& baseName)
{
    if (!sourceButton) {
        return;
    }

    const QVector<Emoji> variants =
        EmojiInfo::skinToneVariantsByName(baseName);
    if (variants.isEmpty()) {
        return;
    }

    auto* popup = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    popup->setObjectName(QStringLiteral("emojiSkinTonePopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setFrameShape(QFrame::StyledPanel);

    auto* layout = new QHBoxLayout(popup);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    for (int tone = 0; tone < variants.size(); ++tone) {
        const Emoji variant = variants.at(tone);
        auto* option = new QPushButton(variant.unicodeString, popup);
        option->setObjectName(QStringLiteral("emojiSkinToneOption"));
        option->setFixedSize(EmojiButtonExtent, EmojiButtonExtent);
        option->setFlat(true);
        option->setFont(EmojiDialogSupport::emojiButtonFont(QFont()));
        option->setToolTip(EmojiSkinTone::descriptionString[tone]);
        connect(option, &QPushButton::clicked, popup,
                [this, popup, variant] {
            emit emojiChosen(variant);
            popup->close();
        });
        layout->addWidget(option);
    }

    popup->adjustSize();
    const QPoint sourceTop = sourceButton->mapToGlobal(QPoint(
        sourceButton->width() / 2, 0));
    QPoint position(
        sourceTop.x() - popup->width() / 2,
        sourceTop.y() - popup->height() - 4);

    if (QScreen* screen = QApplication::screenAt(sourceTop)) {
        const QRect available = screen->availableGeometry();
        position.setX(qBound(
            available.left(),
            position.x(),
            available.right() - popup->width() + 1));
        if (position.y() < available.top()) {
            position.setY(sourceButton->mapToGlobal(
                QPoint(0, sourceButton->height() + 4)).y());
        }
    }

    popup->move(position);
    popup->show();
    popup->raise();
}

} // namespace Mattermost
