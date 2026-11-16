#pragma once

#ifdef MATTERLEAST_VOICE_RECORDING

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class QMediaRecorder;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QAudioInput;
class QMediaCaptureSession;
#else
class QAudioRecorder;
#endif

namespace Mattermost {

// Records directly to the application's voice-draft directory. The upload
// outbox owns the resulting file after finished(path) is emitted; this object
// never deletes a successfully completed recording during widget teardown.
class VoiceMessageRecorder final : public QObject
{
    Q_OBJECT
public:
    explicit VoiceMessageRecorder(QObject* parent = nullptr);
    ~VoiceMessageRecorder() override;

    bool start(QString& error);
    void finish(bool send);
    bool active() const { return active_; }
    bool finishing() const { return finishing_; }
    static bool isOwnedVoiceFile(const QString& path);

signals:
    void elapsedChanged(qint64 elapsedMs);
    void completed(const QString& path, const QString& error);

private:
    void onRecorderStopped();
    void recorderFailed(const QString& error);
    void pollSizeAndTime();
    static QString recordingsDirectory();
    void cleanupFailed();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMediaCaptureSession* session_ = nullptr;
    QAudioInput* input_ = nullptr;
    QMediaRecorder* recorder_ = nullptr;
#else
    QAudioRecorder* recorder_ = nullptr;
#endif
    QTimer meter_;
    QElapsedTimer elapsed_;
    QString path_;
    QString error_;
    bool active_ = false;
    bool finishing_ = false;
    bool cancelled_ = false;
};

} // namespace Mattermost

#endif // MATTERLEAST_VOICE_RECORDING
