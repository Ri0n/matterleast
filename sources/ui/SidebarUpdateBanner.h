#pragma once

#include <QWidget>

class QProgressBar;
class QPushButton;

namespace Mattermost {

// Presentation-only widget: it never initiates update network requests or
// launches the updater itself. The owning window supplies the controller state.
class SidebarUpdateBanner final : public QWidget
{
    Q_OBJECT
public:
    enum class Phase { Downloading, Preparing, Ready, Failed };

    explicit SidebarUpdateBanner(QWidget* parent = nullptr);

    void showUpdate(Phase phase, const QString& buildId,
                    int progressPercent = -1, bool restartQueued = false,
                    const QString& error = {});
    void clearUpdate();

signals:
    void activated();

private:
    QPushButton* _button = nullptr;
    QProgressBar* _progress = nullptr;
};

} // namespace Mattermost
