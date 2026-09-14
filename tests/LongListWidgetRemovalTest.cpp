#include <QtTest>

#include <utility>

#include "chat-area/ThreadTimelineSizing.h"
#include "widgets/LongListWidget.h"

// This test target intentionally compiles only LongListWidget.cpp in CMake.
// Pull in the orthogonal layout-reconciliation translation unit here so the
// identity-remap contract is covered without coupling production source files.
#include "widgets/LongListWidgetLayout.cpp"

namespace {

void settleEvents(int rounds = 12)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

class FixedRow final : public QWidget
{
public:
    explicit FixedRow(QString identity = {}, QWidget* parent = nullptr)
        : QWidget(parent)
        , identity_(std::move(identity))
    {
    }

    const QString& identity() const { return identity_; }
    QSize sizeHint() const override { return QSize(420, 64); }
    QSize minimumSizeHint() const override { return sizeHint(); }

private:
    QString identity_;
};

class TestList final : public Mattermost::LongListWidget
{
public:
    using Mattermost::LongListWidget::LongListWidget;

    void setIdentities(QStringList identities)
    {
        identities_ = std::move(identities);
    }

    void swapIdentities(int first, int second)
    {
        identities_.swapItemsAt(first, second);
    }

    int destroyedCount() const { return destroyedCount_; }

protected:
    QWidget* createItemWidget(int index) override
    {
        const QString identity = index >= 0 && index < identities_.size()
            ? identities_.at(index) : QString();
        return new FixedRow(identity);
    }

    QString itemIdentity(const QWidget* widget) const override
    {
        const auto* row = static_cast<const FixedRow*>(widget);
        return row ? row->identity() : QString();
    }

    int indexOfItemIdentity(const QString& identity) const override
    {
        return identities_.indexOf(identity);
    }

    bool isModelItemAvailable(int index) const override
    {
        return index >= 0 && index < identities_.size()
            && !identities_.at(index).isEmpty();
    }

    void destroyItemWidget(int index, QWidget* widget) override
    {
        ++destroyedCount_;
        Mattermost::LongListWidget::destroyItemWidget(index, widget);
    }

private:
    QStringList identities_;
    int destroyedCount_ = 0;
};

} // namespace

class LongListWidgetRemovalTest : public QObject
{
    Q_OBJECT

private slots:
    void removalBeforeViewportPreservesConcreteRow()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();

        list.scrollToIndex(100, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();
        QWidget* row = list.itemWidget(100);
        QVERIFY(row != nullptr);
        const int yBefore = row->y();

        list.removeItems(0, 10);
        settleEvents();

        QCOMPARE(list.itemCount(), 190);
        QCOMPARE(list.itemWidget(90), row);
        QVERIFY2(qAbs(row->y() - yBefore) <= 2,
                 "Removing older logical rows must shift the same concrete widget without moving it on screen");
    }

    void removingCurrentRowPromotesFollowingIdentity()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();

        list.scrollToIndex(50, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();
        QWidget* following = list.itemWidget(51);
        QVERIFY(following != nullptr);

        list.removeItems(50, 1);
        settleEvents();

        QCOMPARE(list.itemCount(), 199);
        QCOMPARE(list.itemWidget(50), following);
    }

    void removingPhantomOldestPrefixEliminatesBlankTop()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(96);
        list.setItemCount(1070);
        list.setRangeAvailable(3, 9);
        list.show();
        list.scrollToIndex(0, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();

        QVERIFY(list.itemWidget(3) != nullptr);
        QVERIFY(list.itemWidget(0) == nullptr);

        list.removeItems(0, 3);
        settleEvents();

        QCOMPARE(list.itemCount(), 1067);
        QVERIFY(list.itemWidget(0) != nullptr);
        QVERIFY2(qAbs(list.itemWidget(0)->y()) <= 2,
                 "Once the server proves leading logical slots are phantom, the oldest real post must occupy the top");
    }

    void identityRemapKeepsConcreteWidgetsAndViewportLock()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setMaterializationLimit(80);

        QStringList identities;
        for (int index = 0; index < 40; ++index) {
            identities.push_back(QStringLiteral("item-%1").arg(index));
        }
        list.setIdentities(identities);
        list.setItemCount(identities.size());
        list.setRangeAvailable(0, identities.size() - 1);
        list.show();
        settleEvents();

        list.scrollToIndex(20, Mattermost::LongListWidget::Alignment::Center);
        settleEvents();
        QVERIFY(list.lockViewportToItem(20,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents();

        QWidget* target = list.itemWidget(20);
        QWidget* neighbour = list.itemWidget(21);
        QVERIFY(target != nullptr);
        QVERIFY(neighbour != nullptr);
        const int targetY = target->y();
        const int destroyedBefore = list.destroyedCount();

        list.swapIdentities(20, 21);
        list.reconcileItemLayout(20, 21);
        settleEvents();

        QCOMPARE(list.itemWidget(21), target);
        QCOMPARE(list.itemWidget(20), neighbour);
        QCOMPARE(list.destroyedCount(), destroyedBefore);
        QVERIFY2(qAbs(target->y() - targetY) <= 2,
                 "A semantic viewport lock must follow the same physical widget across an index remap");
    }

    void threadTombstoneDoesNotConsumeFollowingReplySlot()
    {
        // root + two live replies = 3 visible rows. After deleting one reply,
        // Mattermost's reply_count drops to one, but the client keeps the
        // deleted reply as a tombstone. The visible thread must therefore still
        // have three rows rather than truncating the surviving last reply.
        QCOMPARE(Mattermost::threadLogicalItemCount(2, 0), 3);
        QCOMPARE(Mattermost::threadLogicalItemCount(1, 1), 3);
        QCOMPARE(Mattermost::threadLogicalItemCount(0, 2), 3);
    }
};

QTEST_MAIN(LongListWidgetRemovalTest)

#include "LongListWidgetRemovalTest.moc"