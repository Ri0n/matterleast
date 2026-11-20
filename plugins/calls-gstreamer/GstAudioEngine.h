#pragma once

#include <functional>
#include <memory>

#include <QJsonObject>
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

    GstAudioEngine(QObject* receiver, DescriptionHandler onDescription,
                   CandidateHandler onCandidate, ErrorHandler onError,
                   ConnectedHandler onConnected, QStringList iceServers);
    ~GstAudioEngine();
    GstAudioEngine(const GstAudioEngine&) = delete;
    GstAudioEngine& operator=(const GstAudioEngine&) = delete;

    static QString checkRuntime();
    void start();
    void receiveDescription(const QJsonObject& description);
    void receiveCandidate(const QJsonObject& candidate);
    void setMicrophoneMuted(bool muted);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
