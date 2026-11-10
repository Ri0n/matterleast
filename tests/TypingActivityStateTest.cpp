#include <QtTest>

#include "backend/TypingActivityState.h"

using namespace Mattermost;

class TypingActivityStateTest : public QObject
{
    Q_OBJECT

private slots:
    void tracksUsersAndContextsIndependently()
    {
        TypingActivityState state;
        const QString channel = QStringLiteral("channel");
        const QString thread = QStringLiteral("thread");
        const QString alice = QStringLiteral("alice");
        const QString bob = QStringLiteral("bob");

        QVERIFY(state.observe(channel, QString(), alice, 100, 7000));
        QVERIFY(!state.observe(channel, QString(), alice, 500, 7000));
        QVERIFY(state.observe(channel, QString(), bob, 800, 7000));
        QVERIFY(state.observe(channel, thread, alice, 1000, 7000));
        QCOMPARE(state.users(channel, QString()), (QStringList {alice, bob}));
        QCOMPARE(state.users(channel, thread), (QStringList {alice}));
        QCOMPARE(state.nextExpiryMs(), qint64(7500));

        QCOMPARE(state.expire(7100), QStringList {});
        QCOMPARE(state.users(channel, QString()), (QStringList {alice, bob}));
        QCOMPARE(state.expire(7500),
                 (QStringList {TypingActivityState::contextKey(channel, QString())}));
        QCOMPARE(state.users(channel, QString()), (QStringList {bob}));
        QCOMPARE(state.users(channel, thread), (QStringList {alice}));
        QCOMPARE(state.nextExpiryMs(), qint64(7800));
    }

    void postingOnlyRemovesMatchingUserAndThread()
    {
        TypingActivityState state;
        QVERIFY(state.observe(QStringLiteral("c"), QString(),
                              QStringLiteral("a"), 0, 7000));
        QVERIFY(state.observe(QStringLiteral("c"), QStringLiteral("t"),
                              QStringLiteral("a"), 0, 7000));
        QVERIFY(state.observe(QStringLiteral("c"), QString(),
                              QStringLiteral("b"), 0, 7000));

        QVERIFY(!state.remove(QStringLiteral("c"), QString(),
                              QStringLiteral("missing")));
        QVERIFY(state.remove(QStringLiteral("c"), QString(),
                             QStringLiteral("a")));
        QCOMPARE(state.users(QStringLiteral("c"), QString()),
                 (QStringList {QStringLiteral("b")}));
        QCOMPARE(state.users(QStringLiteral("c"), QStringLiteral("t")),
                 (QStringList {QStringLiteral("a")}));
    }

    void expiredEventDoesNotRemoveRefreshedUser()
    {
        TypingActivityState state;
        QVERIFY(state.observe(QStringLiteral("c"), QString(),
                              QStringLiteral("a"), 100, 7000));
        QVERIFY(!state.observe(QStringLiteral("c"), QString(),
                               QStringLiteral("a"), 6000, 7000));
        QCOMPARE(state.expire(7100), QStringList {});
        QCOMPARE(state.nextExpiryMs(), qint64(13000));
        QCOMPARE(state.expire(13000),
                 (QStringList {TypingActivityState::contextKey(
                     QStringLiteral("c"), QString())}));
    }

    void inactiveContextIsRemovedCompletely()
    {
        TypingActivityState state;
        QVERIFY(state.observe(QStringLiteral("c"), QString(),
                              QStringLiteral("a"), 100, 7000));
        QVERIFY(state.observe(QStringLiteral("c"), QStringLiteral("t"),
                              QStringLiteral("b"), 100, 7000));
        QVERIFY(state.clearContext(QStringLiteral("c"), QString()));
        QVERIFY(!state.clearContext(QStringLiteral("c"), QString()));
        QCOMPARE(state.users(QStringLiteral("c"), QString()), QStringList {});
        QCOMPARE(state.users(QStringLiteral("c"), QStringLiteral("t")),
                 (QStringList {QStringLiteral("b")}));
        QCOMPARE(state.clear().size(), 1);
        QCOMPARE(state.nextExpiryMs(), std::numeric_limits<qint64>::max());
    }
};

QTEST_GUILESS_MAIN(TypingActivityStateTest)
#include "TypingActivityStateTest.moc"
