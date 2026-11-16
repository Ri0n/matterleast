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

signals:
    void dimensionsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void openMedia();
    void syncPresentation();

    Backend& _backend;
    QString _fileId;
    QString _fileName;
    QUrl _source;
    bool _audio = false;
    Ui::AttachedVideoFile* _ui = nullptr;
    QToolButton* _launch = nullptr;
    QPointer<QWidget> _player;
};

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
