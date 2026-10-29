#include "UserActivityMonitor.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>

#include "Backend.h"
#include "WebSocketConnector.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(MATTERLEAST_HAVE_XSS)
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#endif

namespace Mattermost {

UserActivityMonitor::UserActivityMonitor(Backend& backend, QObject* parent)
    : QObject(parent)
    , _backend(backend)
{
    _clock.start();
    if (auto* application = QCoreApplication::instance())
        application->installEventFilter(this);
    _sampleTimer.setInterval(1000);
    _sampleTimer.setTimerType(Qt::CoarseTimer);
    connect(&_sampleTimer, &QTimer::timeout, this, &UserActivityMonitor::sample);
    _sampleTimer.start();
}

UserActivityMonitor::~UserActivityMonitor()
{
    if (auto* application = QCoreApplication::instance())
        application->removeEventFilter(this);
}

bool UserActivityMonitor::eventFilter(QObject* watched, QEvent* event)
{
    Q_UNUSED(watched);
    if (event) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TabletPress:
            _lastUiInputMs = _clock.elapsed();
            break;
        default:
            break;
        }
    }
    return false;
}

qint64 UserActivityMonitor::systemIdleTimeMs() const
{
#ifdef Q_OS_WIN
    LASTINPUTINFO lastInput {};
    lastInput.cbSize = sizeof(lastInput);
    if (GetLastInputInfo(&lastInput)) {
        // GetLastInputInfo uses a 32-bit tick count; unsigned subtraction also
        // handles one wrap when the system has been up for over 49 days.
        return static_cast<qint64>(
            static_cast<DWORD>(GetTickCount64()) - lastInput.dwTime);
    }
#elif defined(MATTERLEAST_HAVE_XSS)
    // XScreenSaver only observes the real X11 session. An XWayland display
    // under Wayland is not a reliable whole-desktop idle timer.
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        static Display* display = XOpenDisplay(nullptr);
        if (display) {
            XScreenSaverInfo* info = XScreenSaverAllocInfo();
            if (info) {
                const bool ok = XScreenSaverQueryInfo(
                    display, DefaultRootWindow(display), info) != 0;
                const qint64 idle = ok ? static_cast<qint64>(info->idle) : -1;
                XFree(info);
                return idle;
            }
        }
    }
#endif
    return -1;
}

void UserActivityMonitor::sample()
{
    const qint64 now = _clock.elapsed();
    const bool connected = _backend.webSocketConnectionState()
        == WebSocketConnector::ConnectionState::Connected;
    qint64 idle = systemIdleTimeMs();
    if (idle < 0) {
        // Qt cannot query global Wayland/macOS idle time. Do not extrapolate
        // activity from an open window or WebSocket heartbeats.
        idle = _lastUiInputMs < 0
            ? -1 : now - _lastUiInputMs;
    }
    if (_policy.shouldSendActive(now, idle, connected))
        _backend.reportUserActivity(true);
}

} // namespace Mattermost
