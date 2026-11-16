#include "build-config.h"

#if BUILD_MULTIMEDIA

#include "AttachedVideoFile.h"

#include <QEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>

#include <qitevideo.h>

#include "AttachmentMediaOpener.h"
#include "backend/types/BackendFile.h"
#include "ui_AttachedVideoFile.h"

namespace Mattermost {

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
    // The inline widget is a child and will be destroyed with this card.
    // Qite independently owns a popped-out player via its top-level window.
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
    // Limit the size of a single message without limiting a popped-out window.
    // qite rasterizes frames in a regular alien QWidget: no native child window
    // can escape QListWidget's clipping or confuse Wayland compositors.
    player->setMinimumSize(_audio ? QSize(280, 128) : QSize(320, 220));
    player->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    _player = player;
    player->installEventFilter(this);
    connect(player, &QObject::destroyed, this, [this] {
        _player = nullptr;
        syncPresentation();
        emit dimensionsChanged();
    });
    _ui->verticalLayout->addWidget(player);
    player->show();
    syncPresentation();
    emit dimensionsChanged();
}

bool AttachedVideoFile::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _player && event && event->type() == QEvent::ParentChange) {
        syncPresentation();
        emit dimensionsChanged();
    }
    return QWidget::eventFilter(watched, event);
}

void AttachedVideoFile::syncPresentation()
{
    const bool detached = _player && _player->parentWidget() != this;
    const QString title = detached ? tr("Show media window")
        : _audio ? tr("Play audio") : tr("Play video");
    _launch->setText(title);
    _launch->setToolTip(title);
    _launch->setVisible(!_player || detached);
}

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
