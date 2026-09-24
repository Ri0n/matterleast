#include <QtTest>

#include "chat-area/AbstractPostSource.h"

using namespace Mattermost;

namespace {

class DummySource : public AbstractPostSource
{
public:
    using AbstractPostSource::AbstractPostSource;

    int itemCount() const override { return 0; }
    bool isAvailable(int) const override { return false; }
    BackendPost* postAt(int) const override { return nullptr; }
    QString postIdAt(int) const override { return {}; }
    int indexOfPost(const QString&) const override { return -1; }
    void requestRange(int, int, RequestReason, quint64) override {}
};

class DummyDecorator : public DummySource
{
public:
    explicit DummyDecorator(AbstractPostSource& wrapped)
        : wrapped_(&wrapped)
    {
    }

    AbstractPostSource* wrappedSource() const override { return wrapped_; }

private:
    AbstractPostSource* wrapped_ = nullptr;
};

class SelfWrappingSource : public DummySource
{
public:
    AbstractPostSource* wrappedSource() const override
    {
        return const_cast<SelfWrappingSource*>(this);
    }
};

} // namespace

class PostSourceDecoratorChainTest : public QObject
{
    Q_OBJECT

private slots:
    void authoritativeSourceTraversesAllPresentationLayers()
    {
        DummySource authoritative;
        DummyDecorator filtered(authoritative);
        DummyDecorator outbox(static_cast<AbstractPostSource&>(filtered));

        QCOMPARE(authoritative.authoritativeSource(),
                 static_cast<AbstractPostSource*>(&authoritative));
        QCOMPARE(filtered.authoritativeSource(),
                 static_cast<AbstractPostSource*>(&authoritative));
        QCOMPARE(outbox.authoritativeSource(),
                 static_cast<AbstractPostSource*>(&authoritative));
    }

    void malformedCycleCannotLoopForever()
    {
        SelfWrappingSource source;
        QCOMPARE(source.authoritativeSource(),
                 static_cast<AbstractPostSource*>(&source));
    }
};

QTEST_APPLESS_MAIN(PostSourceDecoratorChainTest)
#include "PostSourceDecoratorChainTest.moc"
