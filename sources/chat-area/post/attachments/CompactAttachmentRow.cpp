#include "CompactAttachmentRow.h"

#include <algorithm>

#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QUrl>

#include "AttachmentPresentation.h"
#include "AttachmentSavePaths.h"
#include "Settings.h"
#include "backend/AttachmentService.h"
#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "config/Config.h"
#include "options/MLOptions.h"

namespace Mattermost {

CompactAttachmentRow::CompactAttachmentRow(
    Backend& backend, const BackendFile& file, QWidget* parent)
    : QWidget(parent)
    , _backend(backend)
    , _fileId(file.id)
    , _fileName(file.name)
{
    setObjectName(QStringLiteral("compactAttachmentRow"));
    setFixedHeight(36);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 1, 6, 2);
    layout->setSpacing(7);

    auto* icon = new QLabel(this);
    icon->setFixedSize(20, 20);
    icon->setAlignment(Qt::AlignCenter);
    const QIcon fileIcon = AttachmentPresentation::describeFile(file.name).icon;
    icon->setPixmap(fileIcon.pixmap(QSize(18, 18)));
    layout->addWidget(icon);

    _nameLabel = new QLabel(this);
    _nameLabel->setObjectName(QStringLiteral("compactAttachmentName"));
    _nameLabel->setToolTip(_fileName);
    _nameLabel->setMinimumWidth(0);
    _nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(_nameLabel, 1);

    auto* sizeLabel = new QLabel(
        QLocale::system().formattedDataSize(
            static_cast<qint64>(file.size), 1,
            QLocale::DataSizeTraditionalFormat),
        this);
    sizeLabel->setObjectName(QStringLiteral("compactAttachmentSize"));
    sizeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(sizeLabel);

    const auto makeButton = [this, layout](QStyle::StandardPixmap iconId,
                                          const QString& tip) {
        auto* button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setIcon(style()->standardIcon(iconId));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(26, 26);
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
        return button;
    };
    _saveButton = makeButton(QStyle::SP_DialogSaveButton, tr("Download file"));
    _openButton = makeButton(QStyle::SP_DialogOpenButton, tr("Open file"));
    _cancelButton = makeButton(QStyle::SP_DialogCancelButton, tr("Cancel download"));
    _cancelButton->hide();

    connect(_cancelButton, &QToolButton::clicked, this, [this] {
        if (_downloadReply) {
            _downloadReply->abort();
        }
    });
    connect(_saveButton, &QToolButton::clicked, this, [this] {
        if (_downloadReply) {
            return;
        }
        const QString downloads =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        auto* settings = MLOptions::instance();
        const QDir directory(settings->optionObject<QString>(
            DOWNLOAD_LOCATION, downloads)->value().toString());
        const bool ask = settings->optionObject<bool>(
            DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)->value().toBool();
        QString target = AttachmentSavePaths::unusedPath(directory, _fileName);
        if (ask) {
            target = QFileDialog::getSaveFileName(
                this, tr("Save file as - MatterLeast"), target);
        }
        if (!target.isEmpty()) {
            beginDownload(target, false);
        }
    });
    connect(_openButton, &QToolButton::clicked, this, [this] {
        if (_downloadReply) {
            return;
        }
        if (!_savedPath.isEmpty() && QFileInfo::exists(_savedPath)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(_savedPath));
            return;
        }

        QString name = AttachmentSavePaths::safeName(_fileName);
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        name.insert(dot < 0 ? name.size() : dot, QStringLiteral("-XXXXXX"));
        _temporaryFile.setFileTemplate(Config::tempDirectory().filePath(name));
        if (!_temporaryFile.open()) {
            QMessageBox::warning(this, tr("Open file"), _temporaryFile.errorString());
            return;
        }
        const QString target = _temporaryFile.fileName();
        _temporaryFile.close();
        beginDownload(target, true);
    });

    refreshName();
}

CompactAttachmentRow::~CompactAttachmentRow()
{
    if (_downloadReply) {
        _downloadReply->abort();
    }
}

void CompactAttachmentRow::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    QColor separator = palette().color(QPalette::Mid);
    separator.setAlpha(90);
    painter.setPen(separator);
    painter.drawLine(6, height() - 1, width() - 6, height() - 1);
}

void CompactAttachmentRow::refreshName()
{
    if (!_nameLabel) {
        return;
    }
    // No wrapping: filenames never increase the height of a file row.
    const int available = std::max(16, _nameLabel->width());
    _nameLabel->setText(
        _nameLabel->fontMetrics().elidedText(_fileName, Qt::ElideMiddle, available));
}

void CompactAttachmentRow::beginDownload(const QString& destination, bool openAfter)
{
    if (_downloadReply) {
        return;
    }
    _saveButton->setEnabled(false);
    _openButton->setEnabled(false);
    _cancelButton->show();

    QPointer<CompactAttachmentRow> self(this);
    _downloadReply = AttachmentService::instance(_backend).downloadToFile(
        _fileId, destination,
        [self](qint64 received, qint64 total) {
            if (self && total > 0) {
                self->_cancelButton->setToolTip(
                    self->tr("Cancel download (%1 / %2)")
                        .arg(QLocale::system().formattedDataSize(received))
                        .arg(QLocale::system().formattedDataSize(total)));
            }
        },
        [self, destination, openAfter](const QString& error) {
            if (!self) {
                return;
            }
            self->_downloadReply = nullptr;
            self->_saveButton->setEnabled(true);
            self->_openButton->setEnabled(true);
            self->_cancelButton->hide();
            self->_cancelButton->setToolTip(self->tr("Cancel download"));
            if (!error.isEmpty()) {
                QMessageBox::warning(self, self->tr("Download failed"), error);
                return;
            }
            if (openAfter) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
            } else {
                self->_savedPath = destination;
                self->_saveButton->setToolTip(self->tr("Download again"));
                self->_openButton->setToolTip(self->tr("Open saved file"));
            }
        });
}

} // namespace Mattermost
