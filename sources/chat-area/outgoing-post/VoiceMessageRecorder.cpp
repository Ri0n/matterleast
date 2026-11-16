#include "VoiceMessageRecorder.h"

#ifdef MATTERLEAST_VOICE_RECORDING

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaRecorder>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QAudioInput>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#else
#include <QAudioEncoderSettings>
#include <QAudioRecorder>
#include <QVideoEncoderSettings>
#endif

#include "VoiceRecordingPolicy.h"

namespace Mattermost {
namespace {

constexpr int SpeechBitrate = 64000;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
struct RecordingFormat {
    QMediaFormat format;
    QString extension;
};

RecordingFormat selectFormat()
{
    using Codec = QMediaFormat::AudioCodec;
    struct Candidate {
        QMediaFormat::FileFormat container;
        Codec codec;
        const char* extension;
    };
    // Opus is preferred when the current backend really supports encoding.
    // AAC/M4A and MP3 work on systems without an Opus encoder; no WAV fallback.
    const Candidate candidates[] = {
        {QMediaFormat::Ogg, Codec::Opus, "ogg"},
        {QMediaFormat::Ogg, Codec::Vorbis, "ogg"},
        {QMediaFormat::Mpeg4Audio, Codec::AAC, "m4a"},
        {QMediaFormat::MP3, Codec::MP3, "mp3"},
    };
    for (const Candidate& candidate : candidates) {
        QMediaFormat format(candidate.container);
        format.setAudioCodec(candidate.codec);
        if (format.isSupported(QMediaFormat::Encode)) {
            return {format, QString::fromLatin1(candidate.extension)};
        }
    }
    return {};
}
#else
struct RecordingFormat {
    QString container;
    QString codec;
    QString extension;
};

RecordingFormat selectFormat(QAudioRecorder& recorder)
{
    const QStringList containers = recorder.supportedContainers();
    const QStringList codecs = recorder.supportedAudioCodecs();
    struct Candidate {
        const char* containerToken;
        const char* codecToken;
        const char* extension;
    };
    const Candidate candidates[] = {
        {"ogg", "opus", "ogg"},
        {"ogg", "vorbis", "ogg"},
        {"mp4", "aac", "m4a"},
        {"m4a", "aac", "m4a"},
        {"quicktime", "aac", "m4a"},
        {"mp4", "mpeg", "m4a"},
        {"m4a", "mpeg", "m4a"},
        {"quicktime", "mpeg", "m4a"},
        {"mpeg", "mp3", "mp3"},
        {"mp3", "mp3", "mp3"},
        {"mpeg", "layer 3", "mp3"},
    };
    for (const Candidate& candidate : candidates) {
        for (const QString& container : containers) {
            if (!container.contains(QLatin1String(candidate.containerToken),
                                    Qt::CaseInsensitive)) {
                continue;
            }
            for (const QString& codec : codecs) {
                if (codec.contains(QLatin1String(candidate.codecToken),
                                   Qt::CaseInsensitive)) {
                    return {container, codec,
                            QString::fromLatin1(candidate.extension)};
                }
            }
        }
    }
    return {};
}
#endif

} // namespace

VoiceMessageRecorder::VoiceMessageRecorder(QObject* parent)
    : QObject(parent)
{
    meter_.setParent(this);
    meter_.setInterval(200);
    connect(&meter_, &QTimer::timeout, this,
            &VoiceMessageRecorder::pollSizeAndTime);
}

VoiceMessageRecorder::~VoiceMessageRecorder()
{
    if (active_ || finishing_) {
        cancelled_ = true;
        if (recorder_) {
            recorder_->stop();
        }
        // A still-running encoder owns the file until stop completes.
        // A nonfinalized file is never handed to the uploader.
        if (!path_.isEmpty()) {
            QFile::remove(path_);
        }
    }
}

QString VoiceMessageRecorder::recordingsDirectory()
{
    return QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/voice-recordings");
}

bool VoiceMessageRecorder::isOwnedVoiceFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.fileName().startsWith(QStringLiteral("voice-"))) {
        return false;
    }
    return QDir::cleanPath(info.absolutePath())
        == QDir::cleanPath(recordingsDirectory());
}

bool VoiceMessageRecorder::start(QString& error)
{
    error.clear();
    if (active_ || finishing_) {
        error = tr("An audio recording is already in progress.");
        return false;
    }
    if (!QDir().mkpath(recordingsDirectory())) {
        error = tr("Cannot create the voice recordings directory.");
        return false;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (QMediaDevices::defaultAudioInput().isNull()) {
        error = tr("No microphone is available.");
        return false;
    }
    auto* oldRecorder = recorder_;
    recorder_ = nullptr;
    if (oldRecorder) {
        oldRecorder->deleteLater();
    }
    if (session_) {
        session_->deleteLater();
    }
    if (input_) {
        input_->deleteLater();
    }
    session_ = new QMediaCaptureSession(this);
    input_ = new QAudioInput(QMediaDevices::defaultAudioInput(), this);
    recorder_ = new QMediaRecorder(this);
    session_->setAudioInput(input_);
    session_->setRecorder(recorder_);
    const RecordingFormat selected = selectFormat();
    if (selected.extension.isEmpty() || !recorder_->isAvailable()) {
        error = tr("No supported compressed audio encoder is available.");
        return false;
    }
    recorder_->setMediaFormat(selected.format);
    recorder_->setAudioChannelCount(1);
    recorder_->setAudioSampleRate(48000);
    recorder_->setAudioBitRate(SpeechBitrate);
    recorder_->setQuality(QMediaRecorder::NormalQuality);
    connect(recorder_, &QMediaRecorder::recorderStateChanged, this,
            [this](QMediaRecorder::RecorderState state) {
        if (state == QMediaRecorder::StoppedState) {
            if (finishing_) {
                onRecorderStopped();
            } else if (active_) {
                recorderFailed(tr("Audio recording stopped unexpectedly."));
            }
        }
    });
    connect(recorder_, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error code, const QString& message) {
        if (code != QMediaRecorder::NoError) {
            recorderFailed(message);
        }
    });
#else
    if (recorder_) {
        recorder_->deleteLater();
    }
    recorder_ = new QAudioRecorder(this);
    if (!recorder_->isAvailable() || recorder_->audioInputs().isEmpty()) {
        error = tr("No compatible microphone recording service is available.");
        return false;
    }
    const RecordingFormat selected = selectFormat(*recorder_);
    if (selected.extension.isEmpty()) {
        error = tr("No supported compressed audio encoder is available.");
        return false;
    }
    recorder_->setAudioInput(recorder_->defaultAudioInput().isEmpty()
        ? recorder_->audioInputs().first() : recorder_->defaultAudioInput());
    QAudioEncoderSettings settings;
    settings.setCodec(selected.codec);
    settings.setSampleRate(48000);
    settings.setChannelCount(1);
    settings.setBitRate(SpeechBitrate);
    settings.setQuality(QMultimedia::NormalQuality);
    recorder_->setEncodingSettings(settings, QVideoEncoderSettings(),
                                   selected.container);
    connect(recorder_, &QMediaRecorder::stateChanged, this,
            [this](QMediaRecorder::State state) {
        if (state == QMediaRecorder::StoppedState) {
            if (finishing_) {
                onRecorderStopped();
            } else if (active_) {
                recorderFailed(tr("Audio recording stopped unexpectedly."));
            }
        }
    });
#endif

    path_ = recordingsDirectory() + QStringLiteral("/voice-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QLatin1Char('.') + selected.extension;
    error_.clear();
    cancelled_ = false;
    finishing_ = false;
    active_ = true;
    elapsed_.start();
    emit elapsedChanged(0);
    recorder_->setOutputLocation(QUrl::fromLocalFile(path_));
    recorder_->record();
    if (active_) {
        meter_.start();
    }
    return true;
}

void VoiceMessageRecorder::pollSizeAndTime()
{
    if (!active_) {
        return;
    }
    const qint64 elapsedMs = elapsed_.elapsed();
    emit elapsedChanged(elapsedMs);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (recorder_ && recorder_->error() != QMediaRecorder::NoError) {
        recorderFailed(recorder_->errorString());
        return;
    }
#endif

    // Never load audio into memory. Check the growing output file and leave
    // extra room for final container metadata written by stop().
    const qint64 bytes = QFileInfo(path_).size();
    if (bytes >= VoiceRecordingPolicy::MaxVoiceBytes) {
        recorderFailed(tr("The voice message reached the 100 MiB limit."));
    }
}

void VoiceMessageRecorder::finish(bool send)
{
    if (!active_ || finishing_) {
        return;
    }
    cancelled_ = !send;
    finishing_ = true;
    meter_.stop();
    recorder_->stop();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (recorder_->recorderState() == QMediaRecorder::StoppedState && finishing_) {
#else
    if (recorder_->state() == QMediaRecorder::StoppedState && finishing_) {
#endif
        onRecorderStopped();
    }
}

void VoiceMessageRecorder::recorderFailed(const QString& error)
{
    if (!active_) {
        return;
    }
    error_ = error.isEmpty() ? tr("Microphone recording failed.") : error;
    finish(false);
}

void VoiceMessageRecorder::cleanupFailed()
{
    if (!path_.isEmpty()) {
        QFile::remove(path_);
    }
}

void VoiceMessageRecorder::onRecorderStopped()
{
    if (!finishing_) {
        return;
    }
    finishing_ = false;
    active_ = false;
    meter_.stop();

    QString finishedPath = path_;
    if (recorder_) {
        const QString actualPath = recorder_->actualLocation().toLocalFile();
        if (!actualPath.isEmpty() && isOwnedVoiceFile(actualPath)) {
            finishedPath = actualPath;
        }
        if (error_.isEmpty() && recorder_->error() != QMediaRecorder::NoError) {
            error_ = recorder_->errorString();
        }
    }
    const qint64 bytes = QFileInfo(finishedPath).size();
    const bool valid = !cancelled_ && error_.isEmpty()
        && elapsed_.elapsed() >= VoiceRecordingPolicy::MinimumVoiceMs
        && VoiceRecordingPolicy::withinLimit(bytes);
    if (!valid) {
        if (error_.isEmpty() && !cancelled_) {
            error_ = bytes > VoiceRecordingPolicy::MaxVoiceBytes
                ? tr("The voice message exceeds the 100 MiB limit.")
                : tr("The recording was too short or contains no audio.");
        }
        cleanupFailed();
        if (finishedPath != path_) {
            QFile::remove(finishedPath);
        }
        emit completed({}, cancelled_ ? QString() : error_);
    } else {
        emit completed(finishedPath, {});
    }
    path_.clear();
    error_.clear();
    cancelled_ = false;
}

} // namespace Mattermost

#endif // MATTERLEAST_VOICE_RECORDING
