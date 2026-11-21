#pragma once

#include <QObject>
#include <QPointer>

namespace Mattermost {

// The tray has only one unqualified messageClicked() signal. This tiny
// QObject child of the tray tracks the most recent fallback popup and does
// not keep a manager alive after its account/window has been destroyed.
// Keeping the arbitration independent of QSystemTrayIcon makes the policy
// testable without an OS tray backend.
class TrayFallbackOwner final : public QObject
{
public:
    explicit TrayFallbackOwner(QObject* parent = nullptr)
        : QObject(parent)
    {}

    void claim(QObject* owner) { _owner = owner; }

    bool consume(const QObject* owner)
    {
        if (!owner || _owner != owner)
            return false;
        _owner.clear();
        return true;
    }

    void release(const QObject* owner)
    {
        if (_owner == owner)
            _owner.clear();
    }

    QObject* currentOwner() const { return _owner.data(); }

private:
    QPointer<QObject> _owner;
};

} // namespace Mattermost
