#pragma once

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

    template<typename T>
    MLOptionObject* optionObject(const QString& name,
                                 const T& defaultValue = T())
    {
        const QVariant typedDefault = QVariant::fromValue(defaultValue);
        const int requestedType = typedDefault.userType();

        if (auto* existing = optionObjects.value(name, nullptr)) {
            if (existing->typeId() != requestedType) {
                qFatal("MatterLeast option '%s' was requested with a different type",
                       qPrintable(name));
            }
            return existing;
        }

        const QVariant stored = settings.value(name, typedDefault);
        const T typedValue = stored.value<T>();
        auto* option = new MLOptionObject(
            this, name, QVariant::fromValue(typedValue), requestedType);
        optionObjects.insert(name, option);
        return option;
    }

private:
    friend class MLOptionObject;

    MLOptions();
    void persistValue(const QString& name, const QVariant& value);

    QSettings settings;
    QHash<QString, MLOptionObject*> optionObjects;
};

} // namespace Mattermost
