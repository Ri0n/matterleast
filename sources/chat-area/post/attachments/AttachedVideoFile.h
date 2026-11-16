#pragma once

#include "build-config.h"

#if BUILD_MULTIMEDIA

#include <QPointer>
#include <QUrl>
#include <QWidget>

class QEvent;
class QToolButton;

namespace Ui {
class AttachedVideoFile;
}

namespace Mattermost {

class Backend;
class BackendFile;

// Lazily creates qite's raster media player. A floating player is reparented
// into its own window and stays alive even if the owning message is destroyed.
class AttachedVideoFile final : public QWidget
{
    Q_OBJECT
public:
    explicit AttachedVideoFile(Backend& backend, const BackendFile& file,
                               bool audio, QWidget* parent = nullptr);
    ~AttachedVideoFile() override;

    // Called by the attachment list before measuring a QListWidgetItem.
    // Does not emit dimensionsChanged: the caller owns the layout pass.
    void setAvailableWidth(int width);
    // Shared save action for both the post context menu and detached player.
    void saveAttachmentAs();

signals:
    void dimensionsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void openMedia();
    void syncPresentation();
    void updateInlinePlayerSize();

    Backend& _backend;
    QString _fileId;
    QString _fileName;
    QUrl _source;
    bool _audio = false;
    QSize _videoSize;
    int _availableWidth = 560;
    Ui::AttachedVideoFile* _ui = nullptr;
    QToolButton* _launch = nullptr;
    QPointer<QWidget> _player;
};

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
