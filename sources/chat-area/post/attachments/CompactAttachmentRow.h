#pragma once

#include <QPointer>
#include <QTemporaryFile>
#include <QWidget>

class QLabel;
class QNetworkReply;
class QResizeEvent;
class QToolButton;

namespace Mattermost {

class Backend;
class BackendFile;

// No previews or embedded media controls: one predictable-height file row.
class CompactAttachmentRow final : public QWidget
{
public:
    CompactAttachmentRow(Backend& backend, const BackendFile& file,
                         QWidget* parent = nullptr);
    ~CompactAttachmentRow() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void beginDownload(const QString& destination, bool openAfter);
    void refreshName();

    Backend& _backend;
    QString _fileId;
    QString _fileName;
    QString _savedPath;
    QTemporaryFile _temporaryFile;
    QPointer<QNetworkReply> _downloadReply;
    QLabel* _nameLabel = nullptr;
    QToolButton* _saveButton = nullptr;
    QToolButton* _openButton = nullptr;
    QToolButton* _cancelButton = nullptr;
};

} // namespace Mattermost
