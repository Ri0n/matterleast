#include "SidebarUpdateBanner.h"

#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace Mattermost {

SidebarUpdateBanner::SidebarUpdateBanner(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebarUpdateBanner"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(0);

    _button = new QPushButton(this);
    _button->setObjectName(QStringLiteral("sidebarUpdateButton"));
    _button->setCursor(Qt::PointingHandCursor);
    _button->setMinimumHeight(43);
    _button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _button->setAccessibleName(tr("Install MatterLeast update"));
    // Small, permanent call-to-action: no toast, modal dialog or animation.
    _button->setStyleSheet(QStringLiteral(
        "QPushButton#sidebarUpdateButton {"
        " background-color: #218b52; color: white; border: 0;"
        " border-radius: 7px; padding: 7px 10px; font-weight: 600;"
        " text-align: center; }"
        "QPushButton#sidebarUpdateButton:hover { background-color: #187443; }"
        "QPushButton#sidebarUpdateButton:pressed { background-color: #126038; }"));
    layout->addWidget(_button);

    _progress = new QProgressBar(this);
    _progress->setObjectName(QStringLiteral("sidebarUpdateProgress"));
    _progress->setFixedHeight(3);
    _progress->setTextVisible(false);
    _progress->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 0; background-color: transparent; }"
        "QProgressBar::chunk { background-color: #79d9a5; }"));
    layout->addWidget(_progress);
    _progress->hide();
    connect(_button, &QPushButton::clicked,
            this, &SidebarUpdateBanner::activated);
    hide();
}

void SidebarUpdateBanner::showUpdate(Phase phase, const QString& buildId,
                                     int progressPercent, bool restartQueued,
                                     const QString& error)
{
    QString caption;
    switch (phase) {
    case Phase::Downloading:
        caption = restartQueued ? tr("Restart after download")
                                : tr("Update available");
        if (progressPercent >= 0)
            caption += tr("\nDownloading %1%").arg(progressPercent);
        else
            caption += tr("\nDownloading...");
        _progress->setRange(0, progressPercent >= 0 ? 100 : 0);
        if (progressPercent >= 0)
            _progress->setValue(progressPercent);
        _progress->show();
        break;
    case Phase::Preparing:
        caption = restartQueued ? tr("Restart when ready")
                                : tr("Update available");
        caption += tr("\nPreparing update...");
        _progress->setRange(0, 0);
        _progress->show();
        break;
    case Phase::Ready:
        caption = tr("Update MatterLeast\nRestart to install");
        _progress->hide();
        break;
    case Phase::Failed:
        caption = tr("Update available\nRetry download");
        _progress->hide();
        break;
    }
    _button->setText(caption);
    _button->setAccessibleName(caption.replace(QLatin1Char('\n'), QLatin1Char(' ')));
    _button->setToolTip(error.isEmpty()
        ? tr("MatterLeast update %1").arg(buildId)
        : tr("MatterLeast update %1\n%2").arg(buildId, error));
    show();
}

void SidebarUpdateBanner::clearUpdate()
{
    _progress->hide();
    hide();
}

} // namespace Mattermost
