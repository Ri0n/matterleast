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
};

QTEST_MAIN(MLOptionsTest)

#include "MLOptionsTest.moc"
