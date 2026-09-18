#pragma once

#include <type_traits>

#include <QHash>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>

namespace Mattermost {

class MLOptions;

class MLOptionObject final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariant value READ value WRITE setValue NOTIFY changed)

public:
    QString name() const { return optionName; }
    QVariant value() const { return currentValue; }
    int typeId() const { return optionTypeId; }

public slots:
    void setValue(const QVariant& value);

signals:
    void changed(const QVariant& value);

private:
    friend class MLOptions;

    MLOptionObject(MLOptions* options,
                   QString name,
                   QVariant value,
                   int typeId);

    MLOptions* options = nullptr;
    QString optionName;
    QVariant currentValue;
    int optionTypeId = QMetaType::UnknownType;
};

class MLOptions final : public QObject
{
    Q_OBJECT

public:
    static MLOptions* instance();

    QVariant value(const QString& name,
                   const QVariant& defaultValue) const;

    template<typename T>
    T value(const QString& name, const T& defaultValue = T()) const
    {
        static_assert(!std::is_same_v<std::decay_t<T>, QVariant>,
                      "Use the QVariant value() overload directly");

        const QVariant stored = value(name, QVariant::fromValue(defaultValue));
        return stored.value<T>();
    }

    void setValue(const QString& name, const QVariant& value);

    template<typename T>
    void setValue(const QString& name, const T& value)
    {
        static_assert(!std::is_same_v<std::decay_t<T>, QVariant>,
                      "Use the QVariant setValue() overload directly");

        setValue(name, QVariant::fromValue(value));
    }

    bool contains(const QString& name) const;

    MLOptionObject* optionObject(const QString& name,
                                 const QVariant& defaultValue);

    template<typename T>
    MLOptionObject* optionObject(const QString& name,
                                 const T& defaultValue = T())
    {
        static_assert(!std::is_same_v<std::decay_t<T>, QVariant>,
                      "Use the QVariant optionObject() overload directly");

        return optionObject(name, QVariant::fromValue(defaultValue));
    }

private:
    friend class MLOptionObject;

    MLOptions();
    static void ensureOptionType(const QString& name,
                                 const MLOptionObject* option,
                                 int requestedType);
    void persistValue(const QString& name, const QVariant& value);

    QSettings settings;
    QHash<QString, MLOptionObject*> optionObjects;
};

} // namespace Mattermost
