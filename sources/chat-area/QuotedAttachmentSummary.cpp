#include "QuotedAttachmentSummary.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStyle>

namespace Mattermost {

QuotedAttachmentSummary::QuotedAttachmentSummary(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 1, 0, 1);
    row->setSpacing(5);

    iconLabel = new QLabel(this);
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row->addWidget(iconLabel, 0, Qt::AlignVCenter);

    textLabel = new QLabel(this);
    textLabel->setTextFormat(Qt::PlainText);
    textLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    textLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row->addWidget(textLabel, 1, Qt::AlignVCenter);

    hide();
}

void QuotedAttachmentSummary::setFiles(const QStringList& names)
{
    fileNames = names;
    genericAttachment = false;
    refresh();
}

void QuotedAttachmentSummary::setGenericAttachment(bool visible)
{
    fileNames.clear();
    genericAttachment = visible;
    refresh();
}

void QuotedAttachmentSummary::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && (event->type() == QEvent::FontChange
                  || event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refresh();
    }
}

void QuotedAttachmentSummary::refresh()
{
    if (!iconLabel || !textLabel) {
        return;
    }

    const bool hasFiles = !fileNames.isEmpty();
    const bool visible = hasFiles || genericAttachment;
    setVisible(visible);
    if (!visible) {
        iconLabel->clear();
        textLabel->clear();
        setToolTip(QString());
        return;
    }

    QString displayText;
    QString tooltip;
    QString firstFileName;
    if (hasFiles) {
        firstFileName = fileNames.first();
        displayText = firstFileName;
        if (fileNames.size() > 1) {
            displayText += tr("  +%1 more").arg(fileNames.size() - 1);
        }
        tooltip = fileNames.join(QLatin1Char('\n'));
    } else {
        displayText = tr("Attachment");
        tooltip = displayText;
    }

    QIcon icon;
    if (!firstFileName.isEmpty()) {
        static QMimeDatabase mimeDatabase;
        const QMimeType mimeType =
            mimeDatabase.mimeTypeForFile(firstFileName, QMimeDatabase::MatchExtension);
        icon = QIcon::fromTheme(mimeType.iconName());
        if (icon.isNull()) {
            icon = QIcon::fromTheme(mimeType.genericIconName());
        }
    }
    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    }

    const int extent = std::clamp(fontMetrics().height(), 14, 24);
    iconLabel->setFixedSize(extent, extent);
    iconLabel->setPixmap(icon.pixmap(extent, extent));
    textLabel->setFont(font());
    textLabel->setText(displayText);
    setToolTip(tooltip);
    textLabel->setToolTip(tooltip);
    updateGeometry();
}

} // namespace Mattermost
