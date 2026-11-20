#pragma once

#include <functional>
#include <memory>

#include <QImage>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include "CallsMediaDevice.h"
#include <QString>
#include <QStringList>

class QObject;

// One local PeerConnection per Mattermost Calls participant, with one
// independent RTP decoder branch for each incoming SFU audio track.
// Gst/GLib types and lifetime are private: none cross the Qt plugin ABI.
class GstAudioEngine final
{
public:
    using DescriptionHandler = std::function<void(const QJsonObject&)>;
    using CandidateHandler = std::function<void(const QJsonObject&)>;
    using ErrorHandler = std::function<void(const QString&)>;
    using ConnectedHandler = std::function<void()>;
    using DevicesHandler = std::function<void(const QList<CallsMediaDevice>&,
                                               const QString& selectedMicrophone,
                                               const QString& selectedCamera)>;
    using CameraHandler = std::function<void(bool enabled, const QString& error)>;
    using VideoFrameHandler = std::function<void(const QString& streamMid, const QImage&)>;
    using MediaMapHandler = std::function<void(const QHash<QString, QString>& midToSession)>;
    using VideoStreamHandler = std::function<void(const QString& streamId)>;

    GstAudioEngine(QObject* receiver, DescriptionHandler onDescription,
                   CandidateHandler onCandidate, ErrorHandler onError,
                   ConnectedHandler onConnected, QStringList iceServers,
                   DevicesHandler onDevices = {},
                   CameraHandler onCamera = {},
                   VideoFrameHandler onVideoFrame = {},
                   MediaMapHandler onMediaMap = {},
                   VideoStreamHandler onVideoStream = {},
                   bool enableVideo = false);
    ~GstAudioEngine();
    GstAudioEngine(const GstAudioEngine&) = delete;
    GstAudioEngine& operator=(const GstAudioEngine&) = delete;

    static QString checkRuntime();
    static QString checkVideoRuntime();
    void start();
    void receiveDescription(const QJsonObject& description);
    void receiveCandidate(const QJsonObject& candidate);
    void setMicrophoneMuted(bool muted);
    void requestDevices();
    void setAudioInputDevice(const QString& id);
    void setVideoInputDevice(const QString& id);
    void setCameraEnabled(bool enabled);
    void setCameraPublishing(bool enabled);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
