#pragma once

#include <list>

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include "backend/types/BackendFile.h"

class QNetworkReply;
class QProgressDialog;
class QWidget;

namespace Mattermost {

class Backend;

// The transfer is backend-owned, not PostWidget-owned: virtualized posts can
// leave the viewport while saving multiple files to a chosen directory.
class AttachmentBatchSave final : public QObject
{
public:
    static void chooseFolderAndSave(Backend& backend,
                                    const std::list<BackendFile>& files,
                                    QWidget* parent = nullptr);

private:
    AttachmentBatchSave(Backend& backend, std::list<BackendFile> files,
                        QString directory);
    void startNext();
    void finish();
    void cancel();

    Backend& _backend;
    std::list<BackendFile> _files;
    QString _directory;
    QStringList _errors;
    QSet<QString> _reservedNames;
    QPointer<QProgressDialog> _dialog;
    QPointer<QNetworkReply> _reply;
    int _completed = 0;
    int _saved = 0;
    bool _cancelled = false;
    bool _finished = false;
};

} // namespace Mattermost
