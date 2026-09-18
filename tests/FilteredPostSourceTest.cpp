#include <memory>
#include <vector>

#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendPost.h"
#include "chat-area/FilteredPostSource.h"
#include "widgets/LongListWidget.h"

using namespace Mattermost;

namespace {

QString postId(int index)
{
    return QStringLiteral("post-%1").arg(index);
}

QJsonObject postJson(const QString& id, const QString& type = QString())
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("message"), id},
        {QStringLiteral("type"), type},
        {QStringLiteral("create_at"), 1000.0},
        {QStringLiteral("update_at"), 1000.0},
    };
}

class FakePostSource final : public AbstractPostSource
{
public:
    struct Request {
        int first = -1;
        int last = -1;
        RequestReason reason = RequestReason::Scroll;
        quint64 generation = 0;
    };

    FakePostSource(Storage& storage, int count)
        : storage_(storage)
        , slots_(static_cast<std::size_t>(std::max(0, count)))
    {
    }

    int itemCount() const override
    {
        return static_cast<int>(slots_.size());
    }

    bool isAvailable(int index) const override
    {
        return valid(index) && slots_[index].post != nullptr;
    }

    BackendPost* postAt(int index) const override
    {
        return isAvailable(index) ? slots_[index].post.get() : nullptr;
    }

    QString postIdAt(int index) const override
    {
        return valid(index) ? slots_[index].id : QString();
    }

    int indexOfPost(const QString& id) const override
    {
        if (id.isEmpty()) {
            return -1;
        }
        for (int index = 0; index < itemCount(); ++index) {
            if (slots_[index].id == id) {
                return index;
            }
        }
        return -1;
    }

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override
    {
        lastRequest_ = {first, last, reason, generation};
        emit rangeRequestFinished(first, last);
    }

    const Request& lastRequest() const
    {
        return lastRequest_;
    }

    void resolve(int index,
                 const QString& id,
                 const QString& type = QString(),
                 bool publish = true)
    {
        QVERIFY(valid(index));
        slots_[index].id = id;
        slots_[index].post = std::make_unique<BackendPost>(
            postJson(id, type), storage_);
        if (publish) {
            emit rangeAvailable(index, index);
        }
    }

    void evict(int index)
    {
        QVERIFY(valid(index));
        slots_[index].post.reset();
        emit bodyAvailabilityChanged(index, index, false);
    }

    void insertSlots(int first, int count)
    {
        first = std::max(0, std::min(first, itemCount()));
        count = std::max(0, count);
        for (int i = 0; i < count; ++i) {
            slots_.insert(slots_.begin() + first, Slot {});
        }
        emit itemsInserted(first, count);
    }

    void removeSlots(int first, int count)
    {
        first = std::max(0, std::min(first, itemCount()));
        count = std::max(0, std::min(count, itemCount() - first));
        if (count == 0) {
            return;
        }
        slots_.erase(slots_.begin() + first,
                     slots_.begin() + first + count);
        emit itemsRemoved(first, count);
    }

private:
    struct Slot {
        QString id;
        std::unique_ptr<BackendPost> post;

        Slot() = default;
        Slot(Slot&&) noexcept = default;
        Slot& operator=(Slot&&) noexcept = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

    bool valid(int index) const
    {
        return index >= 0 && index < itemCount();
    }

    Storage& storage_;
    std::vector<Slot> slots_;
    Request lastRequest_;
};

class IdentityRow final : public QWidget
{
public:
    explicit IdentityRow(QString id, QWidget* parent = nullptr)
        : QWidget(parent)
        , id_(std::move(id))
    {
    }

    const QString& id() const { return id_; }

    QSize sizeHint() const override { return QSize(420, 60); }
    QSize minimumSizeHint() const override { return sizeHint(); }

private:
    QString id_;
};

class SourceList final : public LongListWidget
{
public:
    explicit SourceList(FilteredPostSource& source)
        : source_(source)
    {
        setDefaultItemHeight(60);
        setItemCount(source_.itemCount());

        for (int index = 0; index < source_.itemCount(); ++index) {
            if (source_.isAvailable(index)) {
                setRangeAvailable(index, index, true);
            }
        }

        connect(&source_, &AbstractPostSource::itemCountChanged,
                this, [this](int count) { setItemCount(count); });
        connect(&source_, &AbstractPostSource::itemsInserted,
                this, [this](int first, int count) { insertItems(first, count); });
        connect(&source_, &AbstractPostSource::itemsRemoved,
                this, [this](int first, int count) { removeItems(first, count); });
        connect(&source_, &AbstractPostSource::rangeAvailable,
                this, [this](int first, int last) {
                    setRangeAvailable(first, last, true);
                });
        connect(&source_, &AbstractPostSource::bodyAvailabilityChanged,
                this, [this](int first, int last, bool bodyAvailable) {
                    setRangeAvailable(first, last, bodyAvailable);
                });
        connect(&source_, &AbstractPostSource::layoutChanged,
                this, [this](int first, int last) {
                    reconcileItemLayout(first, last);
                });
        connect(&source_, &AbstractPostSource::rangeRequestFinished,
                this, &LongListWidget::finishRangeRequest);
        connect(this, &LongListWidget::rangeRequested,
                this, [this](int first,
                             int last,
                             RequestReason reason,
                             quint64 generation) {
                    source_.requestRange(
                        first,
                        last,
                        static_cast<AbstractPostSource::RequestReason>(reason),
                        generation);
                });
    }

    QWidget* widgetFor(int index) const { return itemWidget(index); }

protected:
    QWidget* createItemWidget(int index) override
    {
        BackendPost* post = source_.postAt(index);
        return post ? new IdentityRow(post->id) : nullptr;
    }

    QString itemIdentity(const QWidget* widget) const override
    {
        const auto* row = dynamic_cast<const IdentityRow*>(widget);
        return row ? row->id() : QString();
    }

    int indexOfItemIdentity(const QString& identity) const override
    {
        return source_.indexOfPost(identity);
    }

    bool isModelItemAvailable(int index) const override
    {
        return source_.isAvailable(index);
    }

private:
    FilteredPostSource& source_;
};

void settleEvents(int rounds = 12)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

class FilteredPostSourceTest : public QObject
{
    Q_OBJECT

private slots:
    void initiallyResolvedRowsAreRemovedFromProjection()
    {
        Backend backend;
        FakePostSource raw(backend.getStorage(), 5);
        for (int i = 0; i < 5; ++i) {
            raw.resolve(i, postId(i), i == 1 ? QStringLiteral("hide") : QString(), false);
        }

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });

        QCOMPARE(raw.itemCount(), 5);
        QCOMPARE(filtered.itemCount(), 4);
        QCOMPARE(filtered.postIdAt(0), postId(0));
        QCOMPARE(filtered.postIdAt(1), postId(2));
        QCOMPARE(filtered.postIdAt(2), postId(3));
        QCOMPARE(filtered.postIdAt(3), postId(4));
        QCOMPARE(filtered.indexOfPost(postId(1)), -1);
        QCOMPARE(raw.indexOfPost(postId(1)), 1);
    }

    void newlyResolvedRejectedRowIsStructurallyRemoved()
    {
        Backend backend;
        FakePostSource raw(backend.getStorage(), 4);
        raw.resolve(0, postId(0), {}, false);
        raw.resolve(1, postId(1), {}, false);
        raw.resolve(3, postId(3), {}, false);

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });

        QCOMPARE(filtered.itemCount(), 4);
        QSignalSpy removed(&filtered, &AbstractPostSource::itemsRemoved);
        QSignalSpy available(&filtered, &AbstractPostSource::rangeAvailable);

        raw.resolve(2, postId(2), QStringLiteral("hide"));

        QCOMPARE(filtered.itemCount(), 3);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toInt(), 2);
        QCOMPARE(removed.at(0).at(1).toInt(), 1);
        QCOMPARE(available.count(), 0);
        QCOMPARE(filtered.indexOfPost(postId(2)), -1);
        QCOMPARE(raw.indexOfPost(postId(2)), 2);
    }

    void rejectionSurvivesBodyEviction()
    {
        Backend backend;
        FakePostSource raw(backend.getStorage(), 2);
        raw.resolve(0, postId(0), QStringLiteral("hide"), false);
        raw.resolve(1, postId(1), {}, false);

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });

        QCOMPARE(filtered.itemCount(), 1);
        raw.evict(0);

        QCOMPARE(filtered.itemCount(), 1);
        QCOMPARE(filtered.indexOfPost(postId(0)), -1);
        QCOMPARE(raw.postIdAt(0), postId(0));
        QVERIFY(!raw.isAvailable(0));
    }

    void requestsTranslateAcrossRejectedRows()
    {
        Backend backend;
        FakePostSource raw(backend.getStorage(), 6);
        for (int i = 0; i < 6; ++i) {
            const bool hidden = i == 1 || i == 4;
            raw.resolve(i, postId(i), hidden ? QStringLiteral("hide") : QString(), false);
        }

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });
        QSignalSpy finished(&filtered, &AbstractPostSource::rangeRequestFinished);

        filtered.requestRange(1, 2,
                              AbstractPostSource::RequestReason::Scroll,
                              17);

        QCOMPARE(raw.lastRequest().first, 2);
        QCOMPARE(raw.lastRequest().last, 3);
        QCOMPARE(raw.lastRequest().generation, quint64(17));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toInt(), 1);
        QCOMPARE(finished.at(0).at(1).toInt(), 2);
    }

    void sourceInsertRemoveKeepsFilteredCoordinates()
    {
        Backend backend;
        FakePostSource raw(backend.getStorage(), 3);
        raw.resolve(0, postId(0), {}, false);
        raw.resolve(1, postId(1), QStringLiteral("hide"), false);
        raw.resolve(2, postId(2), {}, false);

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });

        QCOMPARE(filtered.itemCount(), 2);
        QCOMPARE(filtered.indexOfPost(postId(2)), 1);

        QSignalSpy inserted(&filtered, &AbstractPostSource::itemsInserted);
        QSignalSpy removed(&filtered, &AbstractPostSource::itemsRemoved);

        raw.insertSlots(1, 2);
        QCOMPARE(filtered.itemCount(), 4);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(0).toInt(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 2);
        QCOMPARE(filtered.indexOfPost(postId(2)), 3);

        raw.removeSlots(1, 2);
        QCOMPARE(filtered.itemCount(), 2);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toInt(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 2);
        QCOMPARE(filtered.indexOfPost(postId(2)), 1);
    }

    void filteringRowAboveViewportLockPreservesTargetPosition()
    {
        Backend backend;
        constexpr int Count = 220;
        constexpr int HiddenIndex = 109;
        constexpr int TargetIndex = 110;

        FakePostSource raw(backend.getStorage(), Count);
        for (int i = 0; i < Count; ++i) {
            if (i == HiddenIndex) {
                continue;
            }
            raw.resolve(i, postId(i), {}, false);
        }

        FilteredPostSource filtered(
            raw, [](const BackendPost& post) { return post.type != QStringLiteral("hide"); });
        SourceList list(filtered);
        list.resize(480, 400);
        list.show();

        const QString targetId = postId(TargetIndex);
        const int initialTarget = filtered.indexOfPost(targetId);
        QCOMPARE(initialTarget, TargetIndex);

        list.scrollToIndex(initialTarget, LongListWidget::Alignment::Center);
        settleEvents();
        QTRY_VERIFY(list.widgetFor(initialTarget) != nullptr);
        QVERIFY(list.lockViewportToItem(initialTarget,
                                        LongListWidget::Alignment::Center,
                                        0));
        settleEvents();

        QWidget* targetWidget = list.widgetFor(initialTarget);
        QVERIFY(targetWidget);
        const int targetY = targetWidget->y();

        raw.resolve(HiddenIndex, postId(HiddenIndex), QStringLiteral("hide"));
        settleEvents();

        const int shiftedTarget = filtered.indexOfPost(targetId);
        QCOMPARE(shiftedTarget, TargetIndex - 1);
        QCOMPARE(filtered.itemCount(), Count - 1);
        QCOMPARE(filtered.indexOfPost(postId(HiddenIndex)), -1);
        QCOMPARE(list.widgetFor(shiftedTarget), targetWidget);
        QVERIFY2(qAbs(targetWidget->y() - targetY) <= 2,
                 "Filtering a source row above a viewport lock must not move the semantic target");
    }
};

QTEST_MAIN(FilteredPostSourceTest)
#include "FilteredPostSourceTest.moc"
