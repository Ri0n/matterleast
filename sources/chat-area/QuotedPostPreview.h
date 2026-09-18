#pragma once

#include <QFrame>
#include <functional>
#include <list>

class QEvent;
class QLabel;
class QMouseEvent;
class QTextBrowser;
class QUrl;
class QVBoxLayout;

namespace Mattermost {

class Backend;
class BackendFile;
class BackendPost;
class PostAttachmentList;
class QuotedAttachmentSummary;

class QuotedPostPreview final : public QFrame
{
    Q_OBJECT

public:
    explicit QuotedPostPreview(QWidget* parent = nullptr, int maximumLines = 2);

    void setPost(const BackendPost& post);
    void setPreview(const QString& title,
                    const QString& message,
                    bool hasAttachments = false);
    void setInteractiveAttachments(Backend& backend,
                                   const std::list<BackendFile>& files,
                                   const QString& authorName);
    void setActivatedCallback(std::function<void()> callback);
    void setLinkActivatedCallback(std::function<void(const QUrl&)> callback);

signals:
    void dimensionsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void refreshPalette();
    void refreshText();

    QLabel* authorLabel = nullptr;
    QTextBrowser* messageBrowser = nullptr;
    QuotedAttachmentSummary* attachmentSummary = nullptr;
    PostAttachmentList* attachmentList = nullptr;
    QVBoxLayout* contentLayout = nullptr;
    QFrame* bar = nullptr;
    QString fullText;
    int maximumLines = 2;
    std::function<void()> activatedCallback;
    std::function<void(const QUrl&)> linkActivatedCallback;
};

} // namespace Mattermost
