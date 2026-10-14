#include <memory>

#include <QtTest/QtTest>
#include <QPointer>

#include "chat-area/AttentionFlashLifetime.h"

using namespace Mattermost;

class AttentionFlashLifetimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void recycledRowReleasesItsPostId()
    {
        AttentionFlashLifetime lifetime;
        auto* row = new QObject;
        QVERIFY(lifetime.begin(*row, QStringLiteral("post")));
        QVERIFY(lifetime.isFlashing(QStringLiteral("post")));
        QVERIFY(!lifetime.begin(*row, QStringLiteral("post")));

        delete row; // Virtualized row removed during the highlight.
        QVERIFY(!lifetime.isFlashing(QStringLiteral("post")));

        QObject replacement;
        QVERIFY(lifetime.begin(replacement, QStringLiteral("post")));
        lifetime.finish(QStringLiteral("post"));
        QVERIFY(!lifetime.isFlashing(QStringLiteral("post")));
    }

    void completedAnimationDisconnectsOldRow()
    {
        AttentionFlashLifetime lifetime;
        auto oldRow = std::make_unique<QObject>();
        QObject currentRow;
        QVERIFY(lifetime.begin(*oldRow, QStringLiteral("post")));
        lifetime.finish(QStringLiteral("post"));
        QVERIFY(lifetime.begin(currentRow, QStringLiteral("post")));

        // The earlier row can be destroyed after the new highlight starts.
        // Its finished animation must not erase the new in-flight marker.
        oldRow.reset();
        QVERIFY(lifetime.isFlashing(QStringLiteral("post")));
        lifetime.finish(QStringLiteral("post"));
    }

    void contextDiesBeforeBaseClassDestroysRows()
    {
        struct BaseList : QObject {
            QObject* _row = new QObject(this);
            ~BaseList() override { delete _row; }
        };
        struct ChatList : BaseList {
            std::unique_ptr<AttentionFlashLifetime> _flashes =
                std::make_unique<AttentionFlashLifetime>();

            ~ChatList() override
            {
                // Exactly ChatLogWidget's required order: revoke callbacks
                // before the base class deletes the virtualized row widgets.
                _flashes.reset();
            }
        };

        auto* list = new ChatList;
        QVERIFY(list->_flashes->begin(*list->_row, QStringLiteral("post")));
        QPointer<AttentionFlashLifetime> guard(list->_flashes.get());
        int lateCallbacks = 0;
        auto onAnimationCompleted = [guard, &lateCallbacks] {
            if (guard) {
                ++lateCallbacks;
                guard->finish(QStringLiteral("post"));
            }
        };

        delete list; // Simulates a thread pane being torn down mid-animation.
        QVERIFY(guard.isNull());
        onAnimationCompleted();
        QCOMPARE(lateCallbacks, 0);
    }

    void oldSourceCannotAcknowledgeNewTimeline()
    {
        auto lifetime = std::make_unique<AttentionFlashLifetime>();
        QObject oldRow;
        QVERIFY(lifetime->begin(oldRow, QStringLiteral("same-id")));
        QPointer<AttentionFlashLifetime> oldGuard(lifetime.get());
        // Changing the post source invalidates all old animation callbacks.
        lifetime = std::make_unique<AttentionFlashLifetime>();

        QObject newRow;
        QVERIFY(lifetime->begin(newRow, QStringLiteral("same-id")));
        QVERIFY(oldGuard.isNull());
        if (oldGuard) {
            oldGuard->finish(QStringLiteral("same-id"));
        }
        QVERIFY(lifetime->isFlashing(QStringLiteral("same-id")));
    }
};

QTEST_GUILESS_MAIN(AttentionFlashLifetimeTest)
#include "AttentionFlashLifetimeTest.moc"
