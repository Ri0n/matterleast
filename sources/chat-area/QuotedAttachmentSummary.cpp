#include "QuotedAttachmentSummary.h"

#include "backend/types/BackendFile.h"
#include "post/attachments/AttachmentPresentation.h"

#include <algorithm>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>

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

void QuotedAttachmentSummary::setFiles(const std::list<BackendFile>& files)
{
    fileNames.clear();
    fileNames.reserve(static_cast<int>(files.size()));
    for (const BackendFile& file : files) {
        fileNames.push_back(file.name);
    }
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

    const QIcon icon =
        AttachmentPresentation::describeFile(firstFileName).icon;

    const int extent = std::clamp(fontMetrics().height(), 14, 24);
    iconLabel->setFixedSize(extent, extent);
    if (icon.isNull()) {
        iconLabel->clear();
    } else {
        iconLabel->setPixmap(icon.pixmap(extent, extent));
    }
    textLabel->setFont(font());
    textLabel->setText(displayText);
    setToolTip(tooltip);
    textLabel->setToolTip(tooltip);
    updateGeometry();
}

} // namespace Mattermost
