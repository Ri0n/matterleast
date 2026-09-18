#pragma once

#include <list>

#include <QStringList>
#include <QWidget>

class QEvent;
class QLabel;

namespace Mattermost {

class BackendFile;

/**
 * Compact one-line representation of quoted post attachments.
 *
 * Quote previews should show an actual file affordance instead of leaking the
 * wire fallback marker ("[attachment]") into the UI. The widget deliberately
 * uses only file metadata, so opening a quote never downloads attachment data.
 */
class QuotedAttachmentSummary final : public QWidget
{
public:
    explicit QuotedAttachmentSummary(QWidget* parent = nullptr);

    void setFiles(const std::list<BackendFile>& files);
    void setGenericAttachment(bool visible);

protected:
    void changeEvent(QEvent* event) override;

private:
    void refresh();

    QLabel* iconLabel = nullptr;
    QLabel* textLabel = nullptr;
    QStringList fileNames;
    bool genericAttachment = false;
};

} // namespace Mattermost
