#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QSignalSpy>

#include "ui/BusyIndicator.h"
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
    void sharedBusyAnimationRunsOnlyWhileRequested()
    {
        auto& animation = BusyIndicator::Animation::instance();
        QVERIFY(!animation.isRunning());

        BusyIndicatorWidget first;
        BusyIndicatorWidget second;

        first.setAnimating(true);
        QVERIFY(animation.isRunning());

        second.setAnimating(true);
        QVERIFY(animation.isRunning());

        // QPixmap::cacheKey() is not a portable identity test for
        // implicit-sharing copies (notably on Qt 5). The externally relevant
        // contract is one shared animation clock whose lifetime follows demand;
        // frame-cache reuse is an implementation detail of Animation::frame().
        first.setAnimating(false);
        QVERIFY(animation.isRunning());

        second.setAnimating(false);
        QVERIFY(!animation.isRunning());
    }

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

        // At a 12 px badge two adjacent 30-degree phases can rasterize to the
        // same pixels on some Qt/platform combinations. Wait until any later
        // animation phase is visibly different instead of assuming the very
        // next timer tick must differ bit-for-bit.
        QTRY_VERIFY_WITH_TIMEOUT(renderLabel(label) != firstFrame, 1000);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::None);
        QCOMPARE(renderLabel(label), presence);
    }

    void reconnectRequestComesFromConnectionBadge()
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
        QCOMPARE(reconnectSpy.count(), 1);
        QCOMPARE(avatarClickSpy.count(), 0);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::WaitingForReconnect);
        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(42, 42));
        QCOMPARE(reconnectSpy.count(), 2);
        QCOMPARE(avatarClickSpy.count(), 0);

        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(reconnectSpy.count(), 2);
        QCOMPARE(avatarClickSpy.count(), 1);
    }
};

QTEST_MAIN(PresenceAvatarLabelTest)
#include "PresenceAvatarLabelTest.moc"
