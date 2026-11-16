#include "build-config.h"

#if BUILD_MULTIMEDIA

#include "AttachedVideoFile.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QEvent>
#include <QLayout>
#include <QMenu>
#include <QPointer>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>

#include <algorithm>
#include <utility>

#include <qitevideoframe_p.h>
#include <qitevideo.h>

#include "AttachmentMediaOpener.h"
#include "AttachmentSaveFile.h"
#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "ui_AttachedVideoFile.h"

namespace Mattermost {
namespace {

// Keep the save action with the player, not the message. A detached qite
// window remains fully functional after the original post was pruned.
class AttachmentMediaSaveMenu final : public QObject
{
public:
    AttachmentMediaSaveMenu(Backend& backend, QUrl source,
                            QString fileName, QWidget* player)
        : QObject(player)
        , _backend(&backend)
        , _source(std::move(source))
        , _fileName(std::move(fileName))
    {
        if (auto* frame = player->findChild<QWidget*>(
                QStringLiteral("qite-video-frame"))) {
            frame->installEventFilter(this);
        }
        if (auto* artwork = player->findChild<QWidget*>(
                QStringLiteral("qite-audio-artwork"))) {
            artwork->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event || event->type() != QEvent::ContextMenu) {
            return QObject::eventFilter(watched, event);
        }
        auto* target = qobject_cast<QWidget*>(watched);
        if (!target) return false;
        auto* menu = new QMenu(target);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* save = menu->addAction(AttachedVideoFile::tr("Save attachment as…"));
        save->setEnabled(_backend);
        const QPointer<QWidget> owner(target);
        connect(save, &QAction::triggered, this, [this, owner] {
            if (_backend && owner) saveAs(owner);
        });
        menu->popup(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }

private:
    void saveAs(QWidget* player)
    {
        if (!_backend) return;
        const QString cached =
            AttachmentMediaOpener::instance(*_backend).cachedFilePath(_source);
        AttachmentSaveFile::chooseAndSave(
            *_backend, _source.path().mid(1), _fileName, player->window(), cached);
    }

    QPointer<Backend> _backend;
    QUrl _source;
    QString _fileName;
};

} // namespace

AttachedVideoFile::AttachedVideoFile(
    Backend& backend, const BackendFile& file, bool audio, QWidget* parent)
    : QWidget(parent)
    , _backend(backend)
    , _fileId(file.id)
    , _fileName(file.name)
    , _source(AttachmentMediaOpener::sourceFor(file))
    , _audio(audio)
    , _ui(new Ui::AttachedVideoFile)
{
    _ui->setupUi(this);
    _ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    _ui->verticalLayout->setSpacing(4);
    _ui->videoName->setText(_fileName);
    _ui->videoName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    // The filename must not force a 4K-wide item when the attachment title
    // happens to be longer than its video preview.
    _ui->videoName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _launch = new QToolButton(this);
    _launch->setObjectName(QStringLiteral("attachmentMediaLaunch"));
    _launch->setAutoRaise(true);
    _launch->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _launch->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    _launch->setCursor(Qt::PointingHandCursor);
    _launch->setFocusPolicy(Qt::StrongFocus);
    _ui->verticalLayout->addWidget(_launch, 0, Qt::AlignLeft);
    connect(_launch, &QToolButton::clicked, this, &AttachedVideoFile::openMedia);
    syncPresentation();
}

AttachedVideoFile::~AttachedVideoFile()
{
    // QWidget deletes its children *after* this destructor returns. The
    // QVideoPlayer::destroyed signal would otherwise run syncPresentation()
    // while the button has already been deleted by QWidget's teardown.
    // Detach our observer now, before any child starts destructing.
    if (_player) {
        _player->removeEventFilter(this);
        QObject::disconnect(_player, nullptr, this, nullptr);
    }
    // A detached player belongs to qite's top-level window, not this card.
    delete _ui;
}

void AttachedVideoFile::openMedia()
{
    if (_player) {
        if (_player->parentWidget() != this) {
            QWidget* floating = _player->window();
            floating->show();
            floating->raise();
            floating->activateWindow();
        }
        return;
    }

    QWidget* player = ITEVideoController::createPlayer(
        _source, &AttachmentMediaOpener::instance(_backend), this, {}, 0, _audio);
    new AttachmentMediaSaveMenu(_backend, _source, _fileName, player);
    // qite paints into ordinary QWidget children, not QVideoWidget. The
    // surrounding chat sets a bounded, aspect-correct inline size; detached
    // and fullscreen windows are free to expand.
    player->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    _player = player;
    if (!_audio) {
        if (auto* frame = player->findChild<ITEVideoFrameWidget*>()) {
            connect(frame, &ITEVideoFrameWidget::videoSizeChanged,
                    this, [this](const QSize& size) {
                if (!size.isValid() || _videoSize == size) return;
                _videoSize = size;
                updateInlinePlayerSize();
                if (_player && _player->parentWidget() == this) {
                    _ui->verticalLayout->invalidate();
                    _ui->verticalLayout->activate();
                    updateGeometry();
                    emit dimensionsChanged();
                }
            });
            _videoSize = frame->videoSize();
        }
    }
    player->installEventFilter(this);
    connect(player, &QObject::destroyed, this, [this] {
        _player = nullptr;
        syncPresentation();
        emit dimensionsChanged();
    });
    _ui->verticalLayout->addWidget(player, 0, Qt::AlignLeft);
    updateInlinePlayerSize();
    player->show();
    syncPresentation();
}


void AttachedVideoFile::saveAttachmentAs()
{
    AttachmentSaveFile::chooseAndSave(
        _backend, _fileId, _fileName, this,
        AttachmentMediaOpener::instance(_backend).cachedFilePath(_source));
}

void AttachedVideoFile::setAvailableWidth(int width)
{
    const int bounded = std::max(1, width);
    if (_availableWidth == bounded) return;
    _availableWidth = bounded;
    updateInlinePlayerSize();
}

void AttachedVideoFile::updateInlinePlayerSize()
{
    if (!_player || _player->parentWidget() != this) return;

    // Keep a readable timeline and transport buttons without stretching to
    // the entire chat width. The decoder's frame size determines the picture
    // aspect ratio; a modest 16:9 placeholder is used before the first frame.
    const int maxWidth = std::min(_availableWidth, _audio ? 460 : 640);
    const int minControlsWidth = std::min(maxWidth, _audio ? 340 : 360);
    const int controlsHeight = [this] {
        auto* controls = _player->findChild<QWidget*>(
            QStringLiteral("qite-video-controls"));
        return controls ? controls->sizeHint().height() : 36;
    }();

    QSize videoArea;
    if (_audio) {
        videoArea = QSize(std::max(1, maxWidth), 120);
    } else {
        const QSize frame = _videoSize.isValid() ? _videoSize : QSize(16, 9);
        videoArea = frame.scaled(QSize(std::max(1, maxWidth), 420),
                                 Qt::KeepAspectRatio);
        // Do not enlarge a small source just to fill the preview. The
        // toolbar may still require a wider black backing area.
        if (_videoSize.isValid()) {
            videoArea.setWidth(std::min(videoArea.width(), _videoSize.width()));
            videoArea.setHeight(std::min(videoArea.height(), _videoSize.height()));
        }
    }
    const int width = std::max(minControlsWidth, videoArea.width());
    const int height = videoArea.height() + controlsHeight;
    const QSize target(width, height);
    _player->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    if (_player->size() != target || _player->minimumSize() != target
        || _player->maximumSize() != target) {
        _player->setFixedSize(target);
        _player->updateGeometry();
    }
}

bool AttachedVideoFile::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _player && event && event->type() == QEvent::ParentChange) {
        // qite's floating window takes ownership of the live player. On
        // return it reparents the player, but cannot know our card's layout:
        // insert it back into the QVBoxLayout rather than leaving it floating
        // at an absolute position outside the attachment's size hint.
        syncPresentation();
    }
    return QWidget::eventFilter(watched, event);
}

void AttachedVideoFile::syncPresentation()
{
    const bool inlinePlayer = _player && _player->parentWidget() == this;
    if (inlinePlayer && _ui->verticalLayout->indexOf(_player) < 0) {
        // Qite has just returned the detached player. Its QWidget parent has
        // changed, but it is not yet managed by our QVBoxLayout.
        _ui->verticalLayout->addWidget(_player, 0, Qt::AlignLeft);
        _player->show();
    }

    if (_player) {
        if (inlinePlayer) {
            updateInlinePlayerSize();
        } else {
            // Pop-out/fullscreen must not inherit the chat's fixed geometry.
            _player->setMinimumSize(0, 0);
            _player->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            _player->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        }
    }

    const bool detached = _player && !inlinePlayer;
    const QString title = detached ? tr("Show media window")
        : _audio ? tr("Play audio") : tr("Play video");
    _launch->setText(title);
    _launch->setToolTip(title);
    _launch->setVisible(!inlinePlayer);

    // The attachment row and the outer PostWidget measure this hierarchy
    // immediately after dimensionsChanged(). Make the layout geometry valid
    // first; they must never see the pre-player height.
    _ui->verticalLayout->invalidate();
    _ui->verticalLayout->activate();
    updateGeometry();
    emit dimensionsChanged();
}

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
