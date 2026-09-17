#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one occurrence, got {count}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "sources/ui/PresenceAvatarLabel.cpp",
    '''        && connectionState != ConnectionIndicatorState::None
        && !sourcePixmap.isNull()
        && connectionBadgeRect().adjusted(-BadgeHitMargin, -BadgeHitMargin,
''',
    '''        && connectionState != ConnectionIndicatorState::None
        && connectionBadgeRect().adjusted(-BadgeHitMargin, -BadgeHitMargin,
''')
replace_once(
    "sources/ui/PresenceAvatarLabel.cpp",
    '''    if (connectionState == ConnectionIndicatorState::None || sourcePixmap.isNull()) {
        return;
    }
''',
    '''    if (connectionState == ConnectionIndicatorState::None) {
        return;
    }
''')
replace_once(
    "sources/ui/PresenceAvatarLabel.cpp",
    '''QRectF PresenceAvatarLabel::connectionBadgeRect() const
{
    int avatarSize = std::min(width(), height());
''',
    '''QRectF PresenceAvatarLabel::connectionBadgeRect() const
{
    if (sourcePixmap.isNull()) {
        const qreal extent = std::min<qreal>(16.0, std::min(width(), height()));
        return QRectF((width() - extent) / 2.0,
                      (height() - extent) / 2.0,
                      extent,
                      extent);
    }

    int avatarSize = std::min(width(), height());
''')
replace_once(
    "tests/PresenceAvatarLabelTest.cpp",
    '''private slots:
    void connectionIndicatorAnimatesAndRestoresPresence()
''',
    '''private slots:
    void connectionIndicatorIsVisibleBeforeAvatarLoads()
    {
        PresenceAvatarLabel label;
        label.setFixedSize(48, 48);
        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::Connecting);
        label.show();
        QCoreApplication::processEvents();

        const QImage image = renderLabel(label);
        bool hasPaintedPixel = false;
        for (int y = 12; y < 36 && !hasPaintedPixel; ++y) {
            for (int x = 12; x < 36; ++x) {
                if (image.pixelColor(x, y).alpha() != 0) {
                    hasPaintedPixel = true;
                    break;
                }
            }
        }
        QVERIFY(hasPaintedPixel);
    }

    void connectionIndicatorAnimatesAndRestoresPresence()
''')

Path("tools/apply_connection_indicator_startup_fix.py").unlink()
workflow = Path(".github/workflows/apply-connection-indicator-startup-fix.yml")
if workflow.exists():
    workflow.unlink()
