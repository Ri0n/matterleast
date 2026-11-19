// SPDX-License-Identifier: LGPL-3.0-or-later
#include "CallsWindow.h"

#include <QCloseEvent>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QKeySequence>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

using MatterLeast::PluginApi::CallsParticipant;
using MatterLeast::PluginApi::CallsSnapshot;
using MatterLeast::PluginApi::CallsState;

namespace {

// Draw icons using the active palette rather than depending on icon themes
// that may not ship on all desktop environments. These glyphs are original
// and deliberately limited to controls supported by the audio backend.
QIcon microphoneIcon(const QColor& ink, bool muted)
{
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawRoundedRect(QRectF(20, 9, 8, 22), 4, 4);
    painter.drawArc(QRectF(14, 16, 20, 19), 180 * 16, 180 * 16);
    painter.drawLine(QPointF(24, 35), QPointF(24, 40));
    painter.drawLine(QPointF(18, 40), QPointF(30, 40));
    if (muted) {
        painter.setPen(QPen(ink, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(11, 11), QPointF(37, 37));
    }
    return QIcon(pixmap);
}

QIcon hangupIcon()
{
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath receiver;
    receiver.moveTo(10, 32);
    receiver.cubicTo(17, 19, 31, 19, 38, 32);
    painter.drawPath(receiver);
    painter.drawLine(QPointF(10, 32), QPointF(14, 36));
    painter.drawLine(QPointF(38, 32), QPointF(34, 36));
    return QIcon(pixmap);
}

} // namespace

CallsWindow::CallsWindow(MuteHandler mute, EndHandler end,
                         QString localUserId, QWidget* parent)
    : QDialog(parent)
    , mute_(std::move(mute))
    , end_(std::move(end))
    , localUserId_(std::move(localUserId))
{
    setObjectName(QStringLiteral("callsWindow"));
    setWindowFlags(windowFlags() | Qt::Window);
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(420, 320);
    resize(620, 440);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);

    // Keep the conversation name and transport state separate from the stage:
    // a failure must not erase participants or push the control dock around.
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(2, 0, 2, 0);
    auto* labels = new QVBoxLayout;
    labels->setSpacing(2);
    callTitle_ = new QLabel(tr("Audio call"), this);
    callTitle_->setObjectName(QStringLiteral("callsHeaderTitle"));
    callTitle_->setTextFormat(Qt::PlainText);
    callTitle_->setMinimumWidth(0);
    callTitle_->setWordWrap(true);
    QFont titleFont = callTitle_->font();
    titleFont.setBold(true);
    if (titleFont.pointSize() > 0) titleFont.setPointSize(titleFont.pointSize() + 2);
    callTitle_->setFont(titleFont);
    callSubtitle_ = new QLabel(this);
    callSubtitle_->setObjectName(QStringLiteral("callsHeaderSubtitle"));
    callSubtitle_->setTextFormat(Qt::PlainText);
    labels->addWidget(callTitle_);
    labels->addWidget(callSubtitle_);
    heading->addLayout(labels, 1);
    participantCount_ = new QLabel(this);
    participantCount_->setObjectName(QStringLiteral("callsParticipantCount"));
    participantCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    participantCount_->setTextFormat(Qt::PlainText);
    heading->addWidget(participantCount_);
    layout->addLayout(heading);

    auto* stage = new QFrame(this);
    stage->setObjectName(QStringLiteral("callsStage"));
    auto* stageLayout = new QVBoxLayout(stage);
    stageLayout->setContentsMargins(12, 12, 12, 12);
    stageLayout->setSpacing(0);

    participantsScroll_ = new QScrollArea(stage);
    participantsScroll_->setObjectName(QStringLiteral("callsParticipantsScroll"));
    participantsScroll_->setFrameShape(QFrame::NoFrame);
    participantsScroll_->setWidgetResizable(true);
    participantsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(participantsScroll_);
    content->setObjectName(QStringLiteral("callsParticipants"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    grid_ = new QGridLayout;
    grid_->setObjectName(QStringLiteral("callsTileGrid"));
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(10);
    grid_->setAlignment(Qt::AlignCenter);
    contentLayout->addStretch(1);
    contentLayout->addLayout(grid_);
    contentLayout->addStretch(1);
    participantsScroll_->setWidget(content);
    participantsScroll_->viewport()->installEventFilter(this);
    stageLayout->addWidget(participantsScroll_, 1);

    placeholder_ = new QLabel(stage);
    placeholder_->setObjectName(QStringLiteral("callsPlaceholder"));
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setWordWrap(true);
    placeholder_->setTextFormat(Qt::PlainText);
    stageLayout->addWidget(placeholder_, 1);
    layout->addWidget(stage, 1);

    // Audio-only subset of Psi's one-row call controls. Never display
    // camera/share/device/security controls that the provider cannot use.
    auto* dock = new QFrame(this);
    dock->setObjectName(QStringLiteral("callsControls"));
    auto* controls = new QHBoxLayout(dock);
    controls->setContentsMargins(8, 6, 8, 6);
    controls->setSpacing(8);
    microphone_ = new QToolButton(dock);
    microphone_->setObjectName(QStringLiteral("callsMicrophone"));
    microphone_->setCheckable(true);
    microphone_->setIconSize(QSize(24, 24));
    microphone_->setFixedSize(48, 44);
    microphone_->setCursor(Qt::PointingHandCursor);
    controls->addWidget(microphone_);
    controls->addStretch(1);

    endButton_ = new QPushButton(dock);
    endButton_->setObjectName(QStringLiteral("callsEnd"));
    endButton_->setMinimumSize(126, 44);
    endButton_->setIconSize(QSize(22, 22));
    endButton_->setIcon(hangupIcon());
    endButton_->setCursor(Qt::PointingHandCursor);
    controls->addWidget(endButton_);
    layout->addWidget(dock);

    connect(microphone_, &QToolButton::toggled, this, [this](bool enabled) {
        if (!mute_ || !mute_(!enabled)) {
            const QSignalBlocker guard(microphone_);
            microphone_->setChecked(snapshot_.state == CallsState::Connected
                                    && !snapshot_.microphoneMuted);
        }
        updateMicrophoneIcon();
    });
    // Psi-like keyboard affordance without a global hotkey that would
    // interfere with composing messages in MatterLeast.
    auto* muteShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_M), this);
    connect(muteShortcut, &QShortcut::activated,
            microphone_, &QToolButton::click);
    connect(endButton_, &QPushButton::clicked, this, [this] {
        if (snapshot_.state == CallsState::Failed || snapshot_.state == CallsState::Idle) {
            close();
            return;
        }
        endButton_->setEnabled(false);
        if (end_) end_();
    });
    elapsedTick_.setInterval(1000);
    connect(&elapsedTick_, &QTimer::timeout, this, [this] { updateTimerLabel(); });
    updateAppearance();
    updateSnapshot({});
}

QString CallsWindow::participantLabel(const QString& userId)
{
    // A display-name lookup belongs to the host; opaque IDs are not names.
    return userId.size() > 18 ? userId.left(15) + QChar(0x2026) : userId;
}

void CallsWindow::showForCall(const CallsSnapshot& state, const QString& conversationTitle)
{
    conversationTitle_ = conversationTitle.isEmpty() ? state.channelId : conversationTitle;
    updateSnapshot(state);
    show();
    raise();
    activateWindow();
}

void CallsWindow::updateParticipants(const QList<CallsParticipant>& participants)
{
    QStringList nextOrder;
    QSet<QString> active;
    bool membershipChanged = false;
    for (const CallsParticipant& participant : participants) {
        if (participant.sessionId.isEmpty() || active.contains(participant.sessionId)) continue;
        active.insert(participant.sessionId);
        nextOrder.append(participant.sessionId);
    }

    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (active.contains(it.key())) {
            ++it;
            continue;
        }
        grid_->removeWidget(it.value().frame);
        delete it.value().frame;
        it = tiles_.erase(it);
        membershipChanged = true;
    }

    for (const CallsParticipant& participant : participants) {
        if (participant.sessionId.isEmpty()) continue;
        if (!tiles_.contains(participant.sessionId)) {
            membershipChanged = true;
            ParticipantTile tile;
            tile.frame = new QFrame(participantsScroll_->widget());
            tile.frame->setObjectName(QStringLiteral("callsParticipantTile"));
            tile.frame->setProperty("callsParticipant", true);
            tile.frame->setMinimumSize(148, 132);
            tile.frame->setMaximumWidth(320);
            tile.frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            auto* box = new QVBoxLayout(tile.frame);
            box->setContentsMargins(10, 12, 10, 12);
            box->setSpacing(7);
            tile.avatar = new QLabel(tile.frame);
            tile.avatar->setObjectName(QStringLiteral("callsParticipantAvatar"));
            tile.avatar->setFixedSize(52, 52);
            tile.avatar->setAlignment(Qt::AlignCenter);
            QFont avatarFont = tile.avatar->font();
            avatarFont.setBold(true);
            if (avatarFont.pointSize() > 0) avatarFont.setPointSize(avatarFont.pointSize() + 9);
            tile.avatar->setFont(avatarFont);
            tile.title = new QLabel(tile.frame);
            tile.title->setObjectName(QStringLiteral("callsParticipantName"));
            tile.title->setAlignment(Qt::AlignCenter);
            tile.title->setTextFormat(Qt::PlainText);
            tile.title->setMinimumWidth(0);
            tile.microphone = new QLabel(tile.frame);
            tile.microphone->setObjectName(QStringLiteral("callsParticipantMute"));
            tile.microphone->setAlignment(Qt::AlignCenter);
            tile.microphone->setTextFormat(Qt::PlainText);
            box->addStretch(1);
            box->addWidget(tile.avatar, 0, Qt::AlignHCenter);
            box->addWidget(tile.title);
            box->addWidget(tile.microphone);
            box->addStretch(1);
            tiles_.insert(participant.sessionId, tile);
        }
        auto& tile = tiles_[participant.sessionId];
        tile.frame->setProperty("sessionId", participant.sessionId);
        const bool local = !localUserId_.isEmpty() && participant.userId == localUserId_;
        tile.frame->setProperty("localParticipant", local);
        tile.frame->setAccessibleName(local ? tr("You")
                                            : tr("Participant %1").arg(participant.userId));
        tile.title->setText(local ? tr("You") : participantLabel(participant.userId));
        tile.title->setToolTip(participant.userId);
        tile.avatar->setText(local ? tr("You").left(1)
                                   : participant.userId.isEmpty()
                                       ? QStringLiteral("?")
                                       : participant.userId.left(1).toUpper());
        tile.microphone->setText(participant.microphoneMuted ? tr("Microphone off")
                                                            : tr("Microphone on"));
    }
    tileOrder_ = nextOrder;
    if (membershipChanged) tileColumns_ = 0;
    relayoutTiles();
}

void CallsWindow::relayoutTiles()
{
    if (tileOrder_.isEmpty()) {
        tileColumns_ = 0;
        laidOutOrder_.clear();
        return;
    }

    // Width comes from the actual viewport, not a fixed number of tiles per
    // row. Reposition existing widgets; never recreate them on resize.
    const int viewportWidth = participantsScroll_->viewport()->width();
    const int columns = qMin(tileOrder_.size(),
                             qBound(1, (viewportWidth + 10) / (160 + 10), 4));
    if (columns == tileColumns_ && tileOrder_ == laidOutOrder_) return;

    tileColumns_ = columns;
    laidOutOrder_ = tileOrder_;
    for (const QString& id : tileOrder_) {
        grid_->removeWidget(tiles_.value(id).frame);
    }
    for (int i = 0; i < tileOrder_.size(); ++i) {
        grid_->addWidget(tiles_.value(tileOrder_.at(i)).frame,
                         i / columns, i % columns);
    }
}

void CallsWindow::updateHeader()
{
    const QString title = conversationTitle_.isEmpty() ? snapshot_.channelId
                                                        : conversationTitle_;
    callTitle_->setText(title.isEmpty() ? tr("Audio call") : title);
    callTitle_->setToolTip(title);

    QString status;
    switch (snapshot_.state) {
    case CallsState::Idle:       status = tr("Audio call"); break;
    case CallsState::Connecting: status = tr("Connecting…"); break;
    case CallsState::Connected:  status = tr("Audio call connected"); break;
    case CallsState::Leaving:    status = tr("Leaving call…"); break;
    case CallsState::Failed:     status = tr("Call failed"); break;
    }
    callSubtitle_->setText(status);
    const int count = tileOrder_.size();
    participantCount_->setVisible(count > 0 && snapshot_.state == CallsState::Connected);
    participantCount_->setText(count == 1 ? tr("1 participant")
                                          : tr("%1 participants").arg(count));
}

void CallsWindow::updateSnapshot(const CallsSnapshot& state)
{
    // A new call in a different conversation must not inherit the old timer.
    if (state.state == CallsState::Idle
        || (!snapshot_.channelId.isEmpty() && !state.channelId.isEmpty()
            && snapshot_.channelId != state.channelId)) {
        connectedOnce_ = false;
        elapsedTime_.invalidate();
        elapsedTick_.stop();
    }
    snapshot_ = state;
    if (!snapshot_.channelId.isEmpty()) {
        setWindowTitle(tr("Call — %1").arg(conversationTitle_.isEmpty()
                                                  ? snapshot_.channelId : conversationTitle_));
    }

    if (state.state == CallsState::Connected) {
        if (!connectedOnce_) {
            connectedOnce_ = true;
            elapsedTime_.start();
        }
        if (!elapsedTick_.isActive()) elapsedTick_.start();
    } else {
        elapsedTick_.stop();
    }

    updateParticipants(state.participants);
    updateHeader();
    QString message;
    switch (state.state) {
    case CallsState::Idle:
        message = tr("Audio call");
        break;
    case CallsState::Connecting:
        message = tr("Connecting…");
        break;
    case CallsState::Connected:
        message = tr("Waiting for other participants…");
        break;
    case CallsState::Leaving:
        message = tr("Leaving call…");
        break;
    case CallsState::Failed:
        message = state.errorMessage.isEmpty() ? tr("Call failed")
                                               : state.errorMessage;
        break;
    }
    placeholder_->setText(message);
    const bool showingParticipants = state.state == CallsState::Connected
                                     && !tileOrder_.isEmpty();
    participantsScroll_->setVisible(showingParticipants);
    placeholder_->setVisible(!showingParticipants);
    updateControls();
    if (state.state == CallsState::Idle) hide();
}

void CallsWindow::updateMicrophoneIcon()
{
    // The checked state denotes a live microphone; muted uses a slash.
    const bool enabled = microphone_->isChecked();
    const QColor ink = enabled ? palette().color(QPalette::HighlightedText)
                               : palette().color(QPalette::ButtonText);
    microphone_->setIcon(microphoneIcon(ink, !enabled));
}

void CallsWindow::updateControls()
{
    const bool connected = snapshot_.state == CallsState::Connected;
    {
        const QSignalBlocker guard(microphone_);
        microphone_->setChecked(connected && !snapshot_.microphoneMuted);
    }
    microphone_->setEnabled(connected);
    microphone_->setToolTip(snapshot_.microphoneMuted ? tr("Unmute microphone")
                                                      : tr("Mute microphone"));
    microphone_->setAccessibleName(microphone_->toolTip());
    microphone_->setStatusTip(tr("Ctrl+M to toggle your microphone"));
    updateMicrophoneIcon();

    endButton_->setEnabled(snapshot_.state != CallsState::Leaving);
    if (snapshot_.state == CallsState::Failed || snapshot_.state == CallsState::Idle) {
        endButton_->setText(tr("Close"));
    } else if (connectedOnce_) {
        updateTimerLabel();
    } else {
        endButton_->setText(tr("Cancel"));
    }
    endButton_->setToolTip(snapshot_.state == CallsState::Failed
                              ? tr("Close call window") : tr("End call"));
    endButton_->setAccessibleName(endButton_->toolTip());
}

void CallsWindow::updateTimerLabel()
{
    if (!connectedOnce_ || !elapsedTime_.isValid()) return;
    const qint64 seconds = elapsedTime_.elapsed() / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 remain = seconds % 60;
    endButton_->setText(QStringLiteral("%1:%2:%3")
                            .arg(hours, 2, 10, QLatin1Char('0'))
                            .arg(minutes, 2, 10, QLatin1Char('0'))
                            .arg(remain, 2, 10, QLatin1Char('0')));
}

void CallsWindow::updateAppearance()
{
    if (updatingAppearance_) return;
    updatingAppearance_ = true;
    const QPalette colors = palette();
    const QColor stage = colors.color(QPalette::Base);
    const QColor border = colors.color(QPalette::Mid);
    const QColor tile = colors.color(QPalette::Window);
    const QColor accent = colors.color(QPalette::Highlight);
    const QColor accentInk = colors.color(QPalette::HighlightedText);
    const QColor secondary = colors.color(QPalette::Disabled, QPalette::WindowText);
    const QColor button = colors.color(QPalette::Button);
    const QColor buttonInk = colors.color(QPalette::ButtonText);
    // Selectors are confined to CallsWindow child objects; do not change the
    // application's global stylesheet or override the desktop palette.
    setStyleSheet(QStringLiteral(
        "QFrame#callsStage { background: %1; border: 1px solid %2; border-radius: 12px; }"
        "QFrame#callsParticipantTile { background: %3; border: 1px solid %2; border-radius: 10px; }"
        "QLabel#callsParticipantAvatar { background: %4; color: %5; border-radius: 26px; }"
        "QLabel#callsHeaderSubtitle, QLabel#callsParticipantMute,"
        " QLabel#callsParticipantCount, QLabel#callsPlaceholder { color: %6; }"
        "QFrame#callsControls { background: %7; border: 1px solid %2; border-radius: 10px; }"
        "QToolButton#callsMicrophone { background: %7; color: %8; border: 1px solid %2;"
        " border-radius: 8px; }"
        "QToolButton#callsMicrophone:hover { border-color: %4; }"
        "QToolButton#callsMicrophone:focus { border: 2px solid %4; }"
        "QToolButton#callsMicrophone:checked { background: %4; color: %5; border-color: %4; }"
        "QToolButton#callsMicrophone:disabled { background: %7; border-color: %2; }"
        "QPushButton#callsEnd { background: #b9374b; color: white; border: none;"
        " border-radius: 8px; padding: 6px 12px; font-weight: 600; }"
        "QPushButton#callsEnd:hover { background: #a52e41; }"
        "QPushButton#callsEnd:focus { border: 2px solid %4; }"
        "QPushButton#callsEnd:disabled { background: %2; }")
        .arg(stage.name(), border.name(), tile.name(), accent.name(),
             accentInk.name(), secondary.name(), button.name(), buttonInk.name()));
    updateMicrophoneIcon();
    updatingAppearance_ = false;
}

bool CallsWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (participantsScroll_ && watched == participantsScroll_->viewport()
        && event->type() == QEvent::Resize) {
        relayoutTiles();
    }
    return QDialog::eventFilter(watched, event);
}

void CallsWindow::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        updateAppearance();
    }
}

void CallsWindow::closeEvent(QCloseEvent* event)
{
    // Even a failed call may still own an ICE/GLib media engine. Closing the
    // error window must release it through the provider's normal leave path.
    if (snapshot_.state != CallsState::Idle && snapshot_.state != CallsState::Leaving
        && end_) {
        end_();
    }
    QDialog::closeEvent(event);
}
