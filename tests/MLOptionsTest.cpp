#include <QtTest>

#include <QUuid>

#include "options/MLOptions.h"

using namespace Mattermost;

namespace {

QString uniqueOptionName(const QString& type)
{
    return QStringLiteral("tests/options/") + type + QLatin1Char('/')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

template<typename T>
void verifyOption(const QString& type,
                  const T& initialValue,
                  const T& changedValue)
{
    const QString key = uniqueOptionName(type);
    auto* option = MLOptions::instance()->optionObject<T>(key, initialValue);
    auto* sameOption = MLOptions::instance()->optionObject<T>(key, changedValue);

    QCOMPARE(sameOption, option);
    QCOMPARE(option->value().template value<T>(), initialValue);

    QSignalSpy changed(option, &MLOptionObject::changed);
    option->setValue(QVariant::fromValue(changedValue));
    QCOMPARE(option->value().template value<T>(), changedValue);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.constFirst().constFirst().template value<T>(), changedValue);

    option->setValue(QVariant::fromValue(changedValue));
    QCOMPARE(changed.count(), 1);
}

} // namespace

class MLOptionsTest : public QObject
{
    Q_OBJECT

private slots:
    void boolOption()
    {
        verifyOption<bool>(QStringLiteral("bool"), false, true);
    }

    void intOption()
    {
        verifyOption<int>(QStringLiteral("int"), 100, 125);
    }

    void doubleOption()
    {
        verifyOption<double>(QStringLiteral("double"), 10.5, 12.75);
    }

    void stringOption()
    {
        verifyOption<QString>(
            QStringLiteral("string"),
            QStringLiteral("first"),
            QStringLiteral("second"));
    }

    void variantOverloads()
    {
        const QString key = uniqueOptionName(QStringLiteral("variant"));
        auto* options = MLOptions::instance();

        options->setValue(key, QVariant(QStringLiteral("stored")));
        QCOMPARE(options->value(key, QVariant(QStringLiteral("default"))).toString(),
                 QStringLiteral("stored"));

        auto* observable = options->optionObject(
            key, QVariant(QStringLiteral("default")));
        QSignalSpy changed(observable, &MLOptionObject::changed);

        options->setValue(key, QVariant(QStringLiteral("updated")));
        QCOMPARE(observable->value().toString(), QStringLiteral("updated"));
        QCOMPARE(changed.count(), 1);
    }

    void plainValueRoundTrip()
    {
        const QString key = uniqueOptionName(QStringLiteral("plain-byte-array"));
        const QByteArray first("first");
        const QByteArray second("second");
        auto* options = MLOptions::instance();

        QVERIFY(!options->contains(key));
        options->setValue(key, first);
        QVERIFY(options->contains(key));
        QCOMPARE(options->value<QByteArray>(key), first);

        auto* observable = options->optionObject<QByteArray>(key);
        QCOMPARE(observable->value().toByteArray(), first);

        QSignalSpy changed(observable, &MLOptionObject::changed);
        options->setValue(key, second);
        QCOMPARE(observable->value().toByteArray(), second);
        QCOMPARE(options->value<QByteArray>(key), second);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.constFirst().constFirst().toByteArray(), second);
    }
};

QTEST_MAIN(MLOptionsTest)

#include "MLOptionsTest.moc"
