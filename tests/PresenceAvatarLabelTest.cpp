#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QSignalSpy>

#include "ui/PresenceAvatarLabel.h"

using namespace Mattermost;

namespace {

QImage renderLabel(PresenceAvatarLabel& label)
{
    QImage image(label.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    label.render(&painter);
    return image;
}

} // namespace

class PresenceAvatarLabelTest : public QObject
{
    Q_OBJECT

private slots:
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
    {
        PresenceAvatarLabel label;
        label.setFixedSize(48, 48);
        QPixmap avatar(48, 48);
        avatar.fill(QColor(QStringLiteral("#607D8B")));
        label.setPixmap(avatar);
        label.setStatus(QStringLiteral("online"));
        label.show();
        QCoreApplication::processEvents();

        const QImage presence = renderLabel(label);
        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::Connecting);
        const QImage firstFrame = renderLabel(label);
        QVERIFY(firstFrame != presence);

        QTest::qWait(80);
        const QImage secondFrame = renderLabel(label);
        QVERIFY(secondFrame != firstFrame);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::None);
        QCOMPARE(renderLabel(label), presence);
    }

    void reconnectRequestOnlyComesFromWaitingBadge()
    {
        PresenceAvatarLabel label;
        label.setFixedSize(48, 48);
        QPixmap avatar(48, 48);
        avatar.fill(Qt::gray);
        label.setPixmap(avatar);
        label.show();
        QCoreApplication::processEvents();

        QSignalSpy reconnectSpy(&label, &PresenceAvatarLabel::reconnectRequested);
        QSignalSpy avatarClickSpy(&label, &ClickableLabel::clicked);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::Connecting);
        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(42, 42));
        QCOMPARE(reconnectSpy.count(), 0);
        QCOMPARE(avatarClickSpy.count(), 0);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::WaitingForReconnect);
        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(42, 42));
        QCOMPARE(reconnectSpy.count(), 1);
        QCOMPARE(avatarClickSpy.count(), 0);

        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(reconnectSpy.count(), 1);
        QCOMPARE(avatarClickSpy.count(), 1);
    }
};

QTEST_MAIN(PresenceAvatarLabelTest)
#include "PresenceAvatarLabelTest.moc"
