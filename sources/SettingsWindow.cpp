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

#include "SettingsWindow.h"

#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
#include "update/UpdateTrustPolicy.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFontComboBox>
#include <QFontInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Settings.h"
#include "ai/HermesCredentialStore.h"
#include "backend/emoji/EmojiDefs.h"
#include "options/MLOptions.h"
#include "notifications/RootNotificationPolicy.h"
#include "ui_SettingsWindow.h"

namespace Mattermost {
namespace {

constexpr int MinChatFontPointSize = 8;
constexpr int MaxChatFontPointSize = 30;

QFont fontFromString(const QString& serialized, const QFont& fallback)
{
    QFont result;
    return !serialized.isEmpty() && result.fromString(serialized)
        ? result : fallback;
}

int chatFontPointSize(const QFont& font)
{
    qreal pointSize = font.pointSizeF();
    if (pointSize <= 0.0) {
        pointSize = QFontInfo(font).pointSizeF();
    }
    if (pointSize <= 0.0) {
        pointSize = 10.0;
    }
    return qBound(MinChatFontPointSize, qRound(pointSize), MaxChatFontPointSize);
}

QSpinBox* makeSpinBox(QWidget* parent,
                      int minimum,
                      int maximum,
                      int value,
                      const QString& suffix = QString())
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    spin->setAccelerated(true);
    return spin;
}

QLabel* makeDescription(QWidget* parent, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

} // namespace

SettingsWindow::SettingsWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SettingsWindow)
{
    ui->setupUi(this);
    ui->imageMaxWidthValue->setValidator(new QIntValidator (10, 1000, this));
    ui->imageMaxHeightValue->setValidator(new QIntValidator (10, 1000, this));

    QString defaultDownloadDir (QStandardPaths::writableLocation (QStandardPaths::DownloadLocation));

    auto* options = MLOptions::instance();
    ui->downloadLocationValue->setText(
        options->optionObject<QString>(DOWNLOAD_LOCATION, defaultDownloadDir)
            ->value().toString());
    ui->askLocationCheckBox->setChecked(
        options->optionObject<bool>(DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)
            ->value().toBool());
    ui->imageMaxWidthValue->setText(QString::number(
        options->optionObject<int>(
            DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
            ->value().toInt()));
    ui->imageMaxHeightValue->setText(QString::number(
        options->optionObject<int>(
            DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
            ->value().toInt()));

    // The old form mixed attachment settings with one unlabeled cache-size
    // field. Keep the generated UI stable for now, but move all cache policy to
    // an explicit tab and hide that legacy row.
    ui->label_3->hide();
    ui->label_4->hide();
    ui->cacheSizeMBValue->hide();
    // The legacy designer form has no stretch at its end. Once placed in a
    // full-height tab, QVBoxLayout distributes excess height between every
    // row, leaving enormous gaps between labels and controls.
    ui->verticalLayout_2->setSpacing(12);
    ui->verticalLayout_2->setAlignment(Qt::AlignTop);
    ui->verticalLayout_2->addStretch(1);
    ui->downloads->setFrameShape(QFrame::NoFrame);


    ui->verticalLayout->removeWidget(ui->downloads);
    ui->verticalLayout->removeItem(ui->verticalSpacer);
    delete ui->verticalSpacer;
    ui->verticalSpacer = nullptr;

    auto* tabs = new QTabWidget(this);
    ui->downloads->setParent(tabs);
    tabs->addTab(ui->downloads, tr("Attachments"));

    auto* composerPage = new QWidget(tabs);
    auto* composerLayout = new QVBoxLayout(composerPage);
    composerLayout->setContentsMargins(12, 12, 12, 12);
    composerLayout->setSpacing(8);

    sendWithCtrlEnter = new QCheckBox(
        tr("Send messages with Ctrl+Enter instead of Enter"), composerPage);
    sendWithCtrlEnter->setChecked(
        options->optionObject<bool>(COMPOSER_SEND_WITH_CTRL_ENTER,
                                    COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT)
            ->value().toBool());
    composerLayout->addWidget(sendWithCtrlEnter);
    composerLayout->addWidget(makeDescription(
        composerPage,
        tr("When enabled, Enter inserts a new line and Ctrl+Enter sends the message. "
           "Shift+Enter always inserts a new line.")));
    composerLayout->addStretch(1);
    tabs->addTab(composerPage, tr("Composer"));

    auto* aiPage = new QWidget(tabs);
    auto* aiLayout = new QVBoxLayout(aiPage);
    aiLayout->setContentsMargins(12, 12, 12, 12);
    aiLayout->setSpacing(10);

    _aiEnabled = new QCheckBox(tr("Enable Hermes AI integration"), aiPage);
    _aiEnabled->setObjectName(QStringLiteral("aiHermesEnabled"));
    _aiEnabled->setChecked(options->value<bool>(
        AI_HERMES_ENABLED, AI_HERMES_ENABLED_DEFAULT));
    aiLayout->addWidget(_aiEnabled);
    aiLayout->addWidget(makeDescription(aiPage,
        tr("Off by default. When disabled, MatterLeast does not create an AI "
           "MCP socket or expose the AI thread action. Enabling the integration "
           "only starts the local socket when an AI chat is opened.")));

    auto* aiForm = new QFormLayout;
    auto* executableRow = new QWidget(aiPage);
    auto* executableLayout = new QHBoxLayout(executableRow);
    executableLayout->setContentsMargins(0, 0, 0, 0);
    _aiCommand = new QLineEdit(executableRow);
    _aiCommand->setObjectName(QStringLiteral("aiHermesCommand"));
    _aiCommand->setText(options->value<QString>(
        AI_HERMES_COMMAND, QStringLiteral("hermes")));
    auto* browseHermes = new QPushButton(tr("Browse…"), executableRow);
    executableLayout->addWidget(_aiCommand, 1);
    executableLayout->addWidget(browseHermes);
    aiForm->addRow(tr("Hermes or hook:"), executableRow);
    connect(browseHermes, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Select Hermes CLI or compatible hook"),
            _aiCommand->text());
        if (!path.isEmpty()) _aiCommand->setText(path);
    });

    _aiModel = new QLineEdit(aiPage);
    _aiModel->setObjectName(QStringLiteral("aiHermesModel"));
    _aiModel->setPlaceholderText(tr("e.g. anthropic/claude-sonnet-4.6"));
    _aiModel->setText(options->value<QString>(AI_HERMES_MODEL));
    aiForm->addRow(tr("Model:"), _aiModel);

    _aiProvider = new QComboBox(aiPage);
    _aiProvider->setObjectName(QStringLiteral("aiHermesProvider"));
    _aiProvider->addItem(tr("Use existing Hermes credentials"), QString());
    _aiProvider->addItem(tr("OpenRouter"), QStringLiteral("OPENROUTER_API_KEY"));
    _aiProvider->addItem(tr("Anthropic"), QStringLiteral("ANTHROPIC_API_KEY"));
    _aiProvider->addItem(tr("OpenAI"), QStringLiteral("OPENAI_API_KEY"));
    _aiProvider->addItem(tr("Google Gemini"), QStringLiteral("GEMINI_API_KEY"));
    const QString storedProvider = options->value<QString>(AI_HERMES_PROVIDER_ENV);
    const int providerIndex = _aiProvider->findData(storedProvider);
    _aiProvider->setCurrentIndex(providerIndex < 0 ? 0 : providerIndex);
    aiForm->addRow(tr("API key provider:"), _aiProvider);
    aiLayout->addLayout(aiForm);

    auto* credentials = new QGroupBox(tr("System credential store"), aiPage);
    auto* credentialsLayout = new QVBoxLayout(credentials);
    _aiCredential = new QLineEdit(credentials);
    _aiCredential->setObjectName(QStringLiteral("aiHermesApiKey"));
    _aiCredential->setEchoMode(QLineEdit::Password);
    _aiCredential->setPlaceholderText(tr("Paste a new key (never displayed again)"));
    credentialsLayout->addWidget(_aiCredential);
    auto* credentialButtons = new QHBoxLayout;
    auto* saveCredential = new QPushButton(tr("Save key to keyring"), credentials);
    auto* deleteCredential = new QPushButton(tr("Delete saved key"), credentials);
    credentialButtons->addWidget(saveCredential);
    credentialButtons->addWidget(deleteCredential);
    credentialButtons->addStretch(1);
    credentialsLayout->addLayout(credentialButtons);
    _aiCredentialStatus = makeDescription(credentials,
        tr("No API keys are saved to MatterLeast settings. "
           "Keys are stored separately per provider."));
    _aiCredentialStatus->setObjectName(QStringLiteral("aiHermesCredentialStatus"));
    credentialsLayout->addWidget(_aiCredentialStatus);
    aiLayout->addWidget(credentials);

    const auto refreshAiControls = [this, executableRow, browseHermes,
                                    saveCredential, deleteCredential] {
        const bool active = _aiEnabled->isChecked();
        const bool provider = !_aiProvider->currentData().toString().isEmpty();
        const bool keychain = HermesCredentialStore::available();
        executableRow->setEnabled(active);
        browseHermes->setEnabled(active);
        _aiModel->setEnabled(active);
        _aiProvider->setEnabled(active);
        _aiCredential->setEnabled(active && provider && keychain);
        saveCredential->setEnabled(active && provider && keychain);
        deleteCredential->setEnabled(active && provider && keychain);
        if (!keychain) {
            _aiCredentialStatus->setText(tr(
                "System keyring support is not available in this build. "
                "Configure credentials directly in Hermes instead; "
                "MatterLeast will never store API keys in plaintext."));
        }
    };
    connect(_aiEnabled, &QCheckBox::toggled, this, refreshAiControls);
    connect(_aiProvider, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [refreshAiControls](int) { refreshAiControls(); });
    connect(saveCredential, &QPushButton::clicked, this, [this] {
        const QString provider = _aiProvider->currentData().toString();
        if (_aiCredential->text().isEmpty() || provider.isEmpty()) return;
        const QString key = _aiCredential->text();
        _aiCredential->clear();
        _aiCredentialStatus->setText(tr("Saving key in system keyring…"));
        HermesCredentialStore::write(provider, key, this,
            [this](bool ok, const QString& error) {
                _aiCredentialStatus->setText(ok
                    ? tr("Key stored securely in system keyring.")
                    : tr("Could not save key: %1").arg(error));
            });
    });
    connect(deleteCredential, &QPushButton::clicked, this, [this] {
        const QString provider = _aiProvider->currentData().toString();
        if (provider.isEmpty()) return;
        _aiCredential->clear();
        _aiCredentialStatus->setText(tr("Removing key from system keyring…"));
        HermesCredentialStore::remove(provider, this,
            [this](bool ok, const QString& error) {
                _aiCredentialStatus->setText(ok
                    ? tr("Key removed from system keyring.")
                    : tr("Could not remove key: %1").arg(error));
            });
    });
    refreshAiControls();
    aiLayout->addWidget(makeDescription(aiPage,
        tr("Hermes stream-json mode requires an explicit model. "
           "If no provider is selected, existing Hermes authentication is used. "
           "An API key stored here is only supplied to the launched Hermes "
           "process, never saved in MatterLeast preferences.")));
    aiLayout->addStretch(1);
    tabs->addTab(aiPage, tr("AI"));

#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    if (updateChecksCompiled()) {
        auto* updatesPage = new QWidget(tabs);
        auto* updatesLayout = new QVBoxLayout(updatesPage);
        updatesLayout->setContentsMargins(12, 12, 12, 12);
        updatesLayout->setSpacing(8);

        _updatesAutomatic = new QCheckBox(
            tr("Check for updates automatically"), updatesPage);
        _updatesAutomatic->setObjectName(QStringLiteral("updatesAutomatic"));
        _updatesAutomatic->setChecked(options->value<bool>(
            QStringLiteral("updates/automatic"), true));
        updatesLayout->addWidget(_updatesAutomatic);

        auto* channelLabel = new QLabel(tr("Update channel:"), updatesPage);
        updatesLayout->addWidget(channelLabel);
        _updatesChannel = new QComboBox(updatesPage);
        _updatesChannel->setObjectName(QStringLiteral("updatesChannel"));
        _updatesChannel->addItem(tr("Stable"), QStringLiteral("stable"));
        _updatesChannel->addItem(tr("Nightly"), QStringLiteral("nightly"));
        const QString savedChannel = options->value<QString>(
            QStringLiteral("updates/channel"), defaultUpdateChannelName());
        const int selectedChannel = _updatesChannel->findData(savedChannel);
        _updatesChannel->setCurrentIndex(selectedChannel >= 0 ? selectedChannel : 0);
        updatesLayout->addWidget(_updatesChannel);
        updatesLayout->addWidget(makeDescription(
            updatesPage,
            tr("Nightly builds receive newer, less-tested updates. "
               "Your explicit channel selection is preserved across updates.")));
        updatesLayout->addStretch(1);
        tabs->addTab(updatesPage, tr("Updates"));
    }
#endif

    auto* sidebarPage = new QWidget(tabs);
    auto* sidebarLayout = new QVBoxLayout(sidebarPage);
    sidebarLayout->setContentsMargins(12, 12, 12, 12);
    sidebarLayout->setSpacing(8);

    auto* unreadModeGroup = new QGroupBox(tr("Unread mode"), sidebarPage);
    auto* unreadModeLayout = new QVBoxLayout(unreadModeGroup);
    unreadModeLayout->setSpacing(6);

    unreadModeIgnoreWhileFiltering = new QCheckBox(
        tr("Ignore while filtering"), unreadModeGroup);
    unreadModeIgnoreWhileFiltering->setChecked(
        options->optionObject<bool>(UNREAD_MODE_IGNORE_WHILE_FILTERING,
                                    UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT)
            ->value().toBool());
    unreadModeLayout->addWidget(unreadModeIgnoreWhileFiltering);
    unreadModeLayout->addWidget(makeDescription(
        unreadModeGroup,
        tr("While text is entered in the channel filter, show matching read channels too. "
           "Clearing the text restores Unread mode.")));

    sidebarLayout->addWidget(unreadModeGroup);
    sidebarLayout->addStretch(1);
    tabs->addTab(sidebarPage, tr("Sidebar"));

    auto* notificationsPage = new QWidget(tabs);
    auto* notificationsLayout = new QVBoxLayout(notificationsPage);
    notificationsLayout->setContentsMargins(12, 12, 12, 12);
    notificationsLayout->setSpacing(8);

    auto* channelNotificationsGroup = new QGroupBox(
        tr("Channel message notifications"), notificationsPage);
    auto* channelNotificationsLayout = new QVBoxLayout(channelNotificationsGroup);
    channelNotificationsLayout->setSpacing(8);
    channelNotificationsLayout->addWidget(new QLabel(
        tr("Notify about messages in channels:"), channelNotificationsGroup));
    _channelRootNotificationMode = new QComboBox(channelNotificationsGroup);
    _channelRootNotificationMode->setObjectName(
        QStringLiteral("channelRootNotificationMode"));
    _channelRootNotificationMode->addItem(
        tr("Only when I'm explicitly mentioned"),
        static_cast<int>(ChannelRootNotificationMode::MentionsOnly));
    _channelRootNotificationMode->addItem(
        tr("When I'm mentioned or for all messages in favorite channels"),
        static_cast<int>(ChannelRootNotificationMode::MentionsOrFavorites));
    _channelRootNotificationMode->addItem(
        tr("For all messages in unmuted channels"),
        static_cast<int>(ChannelRootNotificationMode::AllUnmuted));
    const int storedNotificationMode = options->optionObject<int>(
        CHANNEL_ROOT_NOTIFICATION_MODE, CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT)
        ->value().toInt();
    const auto selectedMode = channelRootNotificationModeFromSetting(
        storedNotificationMode);
    _channelRootNotificationMode->setCurrentIndex(
        _channelRootNotificationMode->findData(static_cast<int>(selectedMode)));
    channelNotificationsLayout->addWidget(_channelRootNotificationMode);
    channelNotificationsLayout->addWidget(makeDescription(
        channelNotificationsGroup,
        tr("Applies to new top-level messages in public and private channels. "
           "Muted channels remain silent. Direct/group messages and thread "
           "replies continue to use their existing notification rules.")));
    notificationsLayout->addWidget(channelNotificationsGroup);
    notificationsLayout->addStretch(1);
    tabs->addTab(notificationsPage, tr("Notifications"));


    auto* appearancePage = new QWidget(tabs);
    auto* appearanceLayout = new QVBoxLayout(appearancePage);
    appearanceLayout->setContentsMargins(12, 12, 12, 12);
    appearanceLayout->setSpacing(8);

    auto* chatFontGroup = new QGroupBox(tr("Chat"), appearancePage);
    auto* chatFontForm = new QFormLayout(chatFontGroup);

    auto* chatFontOption = options->optionObject<QString>(
        CHAT_FONT, font().toString());
    originalChatFont = chatFontOption->value().toString();
    QFont chatFont = fontFromString(originalChatFont, font());

    auto* chatFontFamily = new QFontComboBox(chatFontGroup);
    chatFontFamily->setObjectName(QStringLiteral("chatFontFamily"));
    chatFontFamily->setCurrentFont(chatFont);
    chatFontForm->addRow(tr("Font:"), chatFontFamily);

    auto* chatTextSizeEditor = new QWidget(chatFontGroup);
    auto* chatTextSizeLayout = new QHBoxLayout(chatTextSizeEditor);
    chatTextSizeLayout->setContentsMargins(0, 0, 0, 0);
    chatTextSizeLayout->setSpacing(8);

    auto* chatTextSizeSlider = new QSlider(Qt::Horizontal, chatTextSizeEditor);
    chatTextSizeSlider->setObjectName(QStringLiteral("chatTextSizeSlider"));
    chatTextSizeSlider->setRange(MinChatFontPointSize,
                                 MaxChatFontPointSize);
    chatTextSizeSlider->setSingleStep(1);
    chatTextSizeSlider->setPageStep(2);
    chatTextSizeSlider->setMinimumWidth(140);
    chatTextSizeSlider->setValue(chatFontPointSize(chatFont));

    auto* chatTextSizeSpin = new QSpinBox(chatTextSizeEditor);
    chatTextSizeSpin->setObjectName(QStringLiteral("chatTextSizeSpin"));
    chatTextSizeSpin->setRange(MinChatFontPointSize,
                               MaxChatFontPointSize);
    chatTextSizeSpin->setSuffix(tr(" pt"));
    chatTextSizeSpin->setValue(chatTextSizeSlider->value());

    chatTextSizeLayout->addWidget(chatTextSizeSlider, 1);
    chatTextSizeLayout->addWidget(chatTextSizeSpin);
    chatFontForm->addRow(tr("Size:"), chatTextSizeEditor);

    appearanceLayout->addWidget(chatFontGroup);

    auto* codeFontGroup = new QGroupBox(tr("Code blocks"), appearancePage);
    auto* codeFontForm = new QFormLayout(codeFontGroup);
    auto* codeFontOption = options->optionObject<QString>(
        CODE_FONT_FAMILY, QString());
    _originalCodeFontFamily = codeFontOption->value().toString();
    auto* codeFontFamily = new QFontComboBox(codeFontGroup);
    codeFontFamily->setObjectName(QStringLiteral("codeFontFamily"));
    codeFontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    QFont initialCodeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!_originalCodeFontFamily.isEmpty()) {
        initialCodeFont.setFamily(_originalCodeFontFamily);
    }
    codeFontFamily->setCurrentFont(initialCodeFont);
    codeFontForm->addRow(tr("Font:"), codeFontFamily);
    appearanceLayout->addWidget(codeFontGroup);
    connect(codeFontFamily, &QFontComboBox::currentFontChanged, this,
            [codeFontOption](const QFont& selectedFont) {
        codeFontOption->setValue(selectedFont.family());
    });

    appearanceLayout->addWidget(makeDescription(
        appearancePage,
        tr("Font changes are previewed immediately in materialized chat messages. "
           "Cancel restores the previous font.")));
    auto* emojiGroup = new QGroupBox(tr("Emoji"), appearancePage);
    auto* emojiForm = new QFormLayout(emojiGroup);
    emojiDefaultSkinTone_ = new QComboBox(emojiGroup);
    for (int tone = 0; tone < EmojiSkinTone::COUNT; ++tone) {
        emojiDefaultSkinTone_->addItem(
            EmojiSkinTone::descriptionString[tone], tone);
    }
    const int storedSkinTone = qBound(
        0,
        options->optionObject<int>(
            EMOJI_DEFAULT_SKIN_TONE,
            EMOJI_DEFAULT_SKIN_TONE_DEFAULT)
            ->value().toInt(),
        static_cast<int>(EmojiSkinTone::COUNT) - 1);
    emojiDefaultSkinTone_->setCurrentIndex(storedSkinTone);
    emojiDefaultSkinTone_->setToolTip(
        tr("Used by default for emoji that support skin tones. "
           "Hold an emoji in the picker to choose a different tone once."));
    emojiForm->addRow(tr("Default skin tone:"), emojiDefaultSkinTone_);
    appearanceLayout->addWidget(emojiGroup);

    appearanceLayout->addStretch(1);
    tabs->addTab(appearancePage, tr("Appearance"));

    const auto updateChatFontOption =
        [this, chatFontFamily, chatTextSizeSlider, chatFontOption] {
        QFont updated = fontFromString(chatFontOption->value().toString(), font());
        updated.setFamily(chatFontFamily->currentFont().family());
        updated.setPointSizeF(chatTextSizeSlider->value());
        chatFontOption->setValue(updated.toString());
    };

    connect(chatTextSizeSlider, &QSlider::valueChanged, this,
            [chatTextSizeSpin, updateChatFontOption](int value) {
        if (chatTextSizeSpin->value() != value) {
            chatTextSizeSpin->setValue(value);
        }
        updateChatFontOption();
    });
    connect(chatTextSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [chatTextSizeSlider](int value) {
        if (chatTextSizeSlider->value() != value) {
            chatTextSizeSlider->setValue(value);
        }
    });
    connect(chatFontFamily, &QFontComboBox::currentFontChanged, this,
            [updateChatFontOption](const QFont&) {
        updateChatFontOption();
    });

    auto* cacheScroll = new QScrollArea(tabs);
    cacheScroll->setWidgetResizable(true);
    cacheScroll->setFrameShape(QFrame::NoFrame);

    auto* cachePage = new QWidget(cacheScroll);
    auto* cacheLayout = new QVBoxLayout(cachePage);
    cacheLayout->setContentsMargins(12, 12, 12, 12);
    cacheLayout->setSpacing(12);

    auto* admissionDescription = makeDescription(
        cachePage,
        tr("Post caches are admitted by channel recency, not by message activity. "
           "A busy channel that you do not open therefore cannot keep itself hot."));
    cacheLayout->addWidget(admissionDescription);

    auto* attachmentGroup = new QGroupBox(tr("Attachment files"), cachePage);
    auto* attachmentForm = new QFormLayout(attachmentGroup);
    attachmentCacheSizeMB = makeSpinBox(
        attachmentGroup, 16, 102400,
        options->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)->value().toInt(), tr(" MB"));
    attachmentForm->addRow(tr("Maximum disk cache:"), attachmentCacheSizeMB);
    cacheLayout->addWidget(attachmentGroup);

    auto* diskGroup = new QGroupBox(tr("Post cache on disk"), cachePage);
    auto* diskForm = new QFormLayout(diskGroup);
    diskChannelIdleHours = makeSpinBox(
        diskGroup, 1, 720,
        options->optionObject<int>(POST_CACHE_DISK_CHANNEL_IDLE_HOURS, POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT)->value().toInt(),
        tr(" h"));
    diskMaxMB = makeSpinBox(
        diskGroup, 64, 102400,
        options->optionObject<int>(POST_CACHE_DISK_MAX_MB, POST_CACHE_DISK_MAX_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    diskMaxPosts = makeSpinBox(
        diskGroup, 100, 1000000,
        options->optionObject<int>(POST_CACHE_DISK_MAX_POSTS, POST_CACHE_DISK_MAX_POSTS_DEFAULT)->value().toInt());
    diskMaxThreadReplies = makeSpinBox(
        diskGroup, 10, 100000,
        options->optionObject<int>(POST_CACHE_DISK_MAX_THREAD_REPLIES, POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT)->value().toInt());
    diskMaintenanceMinutes = makeSpinBox(
        diskGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_DISK_MAINTENANCE_MINUTES, POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));

    diskForm->addRow(tr("Keep channels opened within:"), diskChannelIdleHours);
    diskForm->addRow(tr("Maximum compressed payload:"), diskMaxMB);
    diskForm->addRow(tr("Maximum posts:"), diskMaxPosts);
    diskForm->addRow(tr("Maximum replies per thread:"), diskMaxThreadReplies);
    diskForm->addRow(tr("Maintenance interval:"), diskMaintenanceMinutes);
    cacheLayout->addWidget(diskGroup);

    auto* memoryGroup = new QGroupBox(tr("Post cache in memory"), cachePage);
    auto* memoryForm = new QFormLayout(memoryGroup);
    memoryChannelIdleMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES, POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));
    memoryHardMB = makeSpinBox(
        memoryGroup, 64, 32768,
        options->optionObject<int>(POST_CACHE_MEMORY_HARD_MB, POST_CACHE_MEMORY_HARD_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    memoryTargetMB = makeSpinBox(
        memoryGroup, 32, memoryHardMB->value(),
        options->optionObject<int>(POST_CACHE_MEMORY_TARGET_MB, POST_CACHE_MEMORY_TARGET_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    memoryPostTtlMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_MEMORY_POST_TTL_MINUTES, POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));
    memorySweepSeconds = makeSpinBox(
        memoryGroup, 5, 3600,
        options->optionObject<int>(POST_CACHE_MEMORY_SWEEP_SECONDS, POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT)->value().toInt(),
        tr(" s"));

    connect(memoryHardMB, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int hardLimit) {
        memoryTargetMB->setMaximum(hardLimit);
        if (memoryTargetMB->value() > hardLimit) {
            memoryTargetMB->setValue(hardLimit);
        }
    });

    memoryForm->addRow(tr("Keep channels opened within:"), memoryChannelIdleMinutes);
    memoryForm->addRow(tr("Hard accounted limit:"), memoryHardMB);
    memoryForm->addRow(tr("Trim target after pressure:"), memoryTargetMB);
    memoryForm->addRow(tr("Cold post idle TTL:"), memoryPostTtlMinutes);
    memoryForm->addRow(tr("Sweep interval:"), memorySweepSeconds);
    cacheLayout->addWidget(memoryGroup);

    cacheLayout->addWidget(makeDescription(
        cachePage,
        tr("SQLite page size, WAL mode and bounded vacuum details are implementation "
           "invariants rather than cache-policy knobs and remain internal.")));
    cacheLayout->addStretch(1);

    cacheScroll->setWidget(cachePage);
    tabs->addTab(cacheScroll, tr("Cache"));
    ui->verticalLayout->insertWidget(0, tabs, 1);

    connect (ui->downloadLocationButton, &QPushButton::clicked, [this] {
        QDir defaultDir (ui->downloadLocationValue->text());

        if (!defaultDir.exists()) {
            defaultDir = QDir::home();
        }
        ui->downloadLocationValue->setText (QFileDialog::getExistingDirectory (this, "Select destination directory", defaultDir.absolutePath()));
    });
}

SettingsWindow::~SettingsWindow()
{
    delete ui;
}

void SettingsWindow::reject()
{
    MLOptions::instance()
        ->optionObject<QString>(CHAT_FONT, font().toString())
        ->setValue(originalChatFont);
    MLOptions::instance()
        ->optionObject<QString>(CODE_FONT_FAMILY, QString())
        ->setValue(_originalCodeFontFamily);
    QDialog::reject();
}

void SettingsWindow::applyNewSettings ()
{
    auto* options = MLOptions::instance();
#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    if (_updatesAutomatic && _updatesChannel) {
        options->optionObject<bool>(QStringLiteral("updates/automatic"), true)
            ->setValue(_updatesAutomatic->isChecked());
        options->optionObject<QString>(QStringLiteral("updates/channel"),
                                      defaultUpdateChannelName())
            ->setValue(_updatesChannel->currentData().toString());
    }
#endif
    options->optionObject<QString>(
        DOWNLOAD_LOCATION,
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
        ->setValue(ui->downloadLocationValue->text());
    options->optionObject<bool>(DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)
        ->setValue(ui->askLocationCheckBox->isChecked());
    options->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
        ->setValue(ui->imageMaxWidthValue->text().toInt());
    options->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
        ->setValue(ui->imageMaxHeightValue->text().toInt());
    options->optionObject<QString>(AI_HERMES_COMMAND, QStringLiteral("hermes"))
        ->setValue(_aiCommand->text().trimmed());
    options->optionObject<QString>(AI_HERMES_MODEL, QString())
        ->setValue(_aiModel->text().trimmed());
    options->optionObject<QString>(AI_HERMES_PROVIDER_ENV, QString())
        ->setValue(_aiProvider->currentData().toString());
    options->optionObject<bool>(AI_HERMES_ENABLED, AI_HERMES_ENABLED_DEFAULT)
        ->setValue(_aiEnabled->isChecked());
    options->optionObject<bool>(
        COMPOSER_SEND_WITH_CTRL_ENTER,
        COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT)
        ->setValue(sendWithCtrlEnter->isChecked());
    options->optionObject<bool>(
        UNREAD_MODE_IGNORE_WHILE_FILTERING,
        UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT)
        ->setValue(unreadModeIgnoreWhileFiltering->isChecked());
    options->optionObject<int>(
        CHANNEL_ROOT_NOTIFICATION_MODE, CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT)
        ->setValue(_channelRootNotificationMode->currentData().toInt());

    options->optionObject<int>(
        EMOJI_DEFAULT_SKIN_TONE,
        EMOJI_DEFAULT_SKIN_TONE_DEFAULT)
        ->setValue(emojiDefaultSkinTone_->currentData().toInt());

    options->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)
        ->setValue(attachmentCacheSizeMB->value());
    options->optionObject<int>(
        POST_CACHE_DISK_CHANNEL_IDLE_HOURS,
        POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT)
        ->setValue(diskChannelIdleHours->value());
    options->optionObject<int>(POST_CACHE_DISK_MAX_MB, POST_CACHE_DISK_MAX_MB_DEFAULT)
        ->setValue(diskMaxMB->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAX_POSTS, POST_CACHE_DISK_MAX_POSTS_DEFAULT)
        ->setValue(diskMaxPosts->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAX_THREAD_REPLIES,
        POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT)
        ->setValue(diskMaxThreadReplies->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAINTENANCE_MINUTES,
        POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT)
        ->setValue(diskMaintenanceMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES,
        POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT)
        ->setValue(memoryChannelIdleMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_HARD_MB, POST_CACHE_MEMORY_HARD_MB_DEFAULT)
        ->setValue(memoryHardMB->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_TARGET_MB, POST_CACHE_MEMORY_TARGET_MB_DEFAULT)
        ->setValue(memoryTargetMB->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_POST_TTL_MINUTES,
        POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT)
        ->setValue(memoryPostTtlMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_SWEEP_SECONDS,
        POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT)
        ->setValue(memorySweepSeconds->value());
}

} /* namespace Mattermost */
