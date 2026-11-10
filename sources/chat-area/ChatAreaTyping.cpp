#include "ChatArea.h"
#include "ChatLogWidget.h"
#include "TypingIndicatorToast.h"

#include <algorithm>

#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QTextEdit>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::setupTypingOverlay()
{
    auto* viewport = ui->listWidget->viewport();
    _typingToast = new TypingIndicatorToast(viewport);
    _typingOpacity = new QGraphicsOpacityEffect(_typingToast);
    _typingOpacity->setOpacity(0.0);
    _typingToast->setGraphicsEffect(_typingOpacity);

    _typingFade = new QPropertyAnimation(_typingOpacity, "opacity", this);
    _typingFade->setDuration(160);
    connect(_typingFade, &QPropertyAnimation::finished, this, [this] {
        if (!_typingVisibleDesired && _typingToast) {
            _typingToast->hide();
        }
    });

    viewport->installEventFilter(this);
    connect(ui->listWidget->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { updateTypingOverlay(); });
    connect(ui->listWidget->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this] { updateTypingOverlay(); });
    connect(ui->listWidget, &LongListWidget::visibleRangeChanged,
            this, [this] { updateTypingOverlay(); });
    connect(ui->listWidget, &LongListWidget::userScrollStarted,
            this, [this] { updateTypingOverlay(); });

    connect(&backend, &Backend::typingChanged, this,
            [this](const QString& channelId, const QString& parentId) {
        if (channelId == channel.id && parentId == root_id) {
            updateTypingOverlay();
        }
    });

    // Paste and IME commits also change the QTextEdit. Programmatic draft
    // restoration and message editing must not announce a new post.
    connect(ui->outgoingPostCreator, &QTextEdit::textChanged, this, [this] {
        auto* editor = ui->outgoingPostCreator;
        if (editor->hasFocus() && !editor->editingPost()
            && !editor->toPlainText().trimmed().isEmpty()) {
            backend.publishUserTyping(channel.id, root_id, channel.member_count);
        }
    });
    positionTypingOverlay();
}

void ChatArea::positionTypingOverlay()
{
    if (!_typingToast || !ui || !ui->listWidget) {
        return;
    }

    QWidget* viewport = ui->listWidget->viewport();
    const int available = viewport->width() - 24;
    if (available < 92) {
        _typingToast->hide();
        return;
    }

    const int width = std::min(available, _typingToast->preferredWidth());
    _typingToast->setGeometry(viewport->width() - width - 12, 10, width, 36);
    if (_typingVisibleDesired && !_typingToast->isVisible()) {
        _typingToast->show();
        _typingToast->raise();
    }
}

void ChatArea::setTypingOverlayVisible(bool visible, bool immediate)
{
    if (!_typingToast || !_typingOpacity || !_typingFade) {
        return;
    }
    if (immediate) {
        _typingVisibleDesired = false;
        _typingFade->stop();
        _typingOpacity->setOpacity(0.0);
        _typingToast->hide();
        return;
    }
    if (visible == _typingVisibleDesired) {
        return;
    }

    _typingVisibleDesired = visible;
    _typingFade->stop();
    if (visible) {
        positionTypingOverlay();
        _typingToast->show();
        _typingToast->raise();
    }

    _typingFade->setDuration(visible ? 150 : 230);
    _typingFade->setEasingCurve(visible
        ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    _typingFade->setStartValue(_typingOpacity->opacity());
    _typingFade->setEndValue(visible ? 1.0 : 0.0);
    _typingFade->start();
}

void ChatArea::updateTypingOverlay()
{
    if (!_typingToast || !ui || !ui->listWidget) {
        return;
    }
    if (!ui->listWidget->isAtEnd() || !isVisible()
        || !ui->listWidget->isVisible()) {
        // Scrolling into history never leaves the indicator over old text.
        setTypingOverlayVisible(false, true);
        return;
    }

    const QStringList ids = backend.typingUsers(channel.id, root_id);
    if (ids.isEmpty()) {
        // Keep the last label in place during the short fade-out.
        setTypingOverlayVisible(false);
        return;
    }

    QStringList names;
    for (const QString& id : ids) {
        const BackendUser* user = backend.getStorage().getUserById(id);
        names.append(user ? user->getDisplayName() : id.left(8));
    }

    QString message;
    if (names.size() == 1) {
        message = tr("%1 is typing…").arg(names.first());
    } else if (names.size() == 2) {
        message = tr("%1 and %2 are typing…").arg(names.at(0), names.at(1));
    } else {
        message = tr("%1 and %2 others are typing…")
                      .arg(names.first()).arg(names.size() - 1);
    }

    _typingToast->setMessage(message);
    positionTypingOverlay();
    setTypingOverlayVisible(true);
}

} // namespace Mattermost
