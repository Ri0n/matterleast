#pragma once

#include <QString>
#include <QtGlobal>
#include <QWidget>

class QEvent;
class QTextDocumentFragment;
class QVBoxLayout;

namespace Mattermost {

class MessageContentWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MessageContentWidget(QWidget* parent = nullptr);

    void setMessage(const QString& message);
    void clear();
    QString selectedText() const;
    void clearSelection();

signals:
    void linkHovered(const QString& link);
    void linkDragRequested(const QString& link);
    void dimensionsChanged();
    void paletteRefreshCompleted();

protected:
    void changeEvent(QEvent* event) override;

private:
    void clearContent();
    void applyChatFont(const QString& serializedFont);
    void addRichText(const QString& html);
    void addQuote(const QString& markdown);
    void scheduleDimensionsChanged();

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    void addRichTextFragment(const QTextDocumentFragment& fragment);
    void addMarkdownContent(const QString& message);
    void addCodeBlock(const QString& code, const QString& language);
#endif

    QVBoxLayout* contentLayout;
    bool dimensionsChangePending = false;
    bool paletteRefreshPending = false;
    QString _sourceMessage;
    bool _jumboEmojiMessage = false;
};

} // namespace Mattermost
