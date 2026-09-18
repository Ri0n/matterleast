#include "MLOptions.h"

#include <utility>

#include <QDebug>

namespace Mattermost {

MLOptionObject::MLOptionObject(MLOptions* options,
                               QString name,
                               QVariant value,
                               int typeId)
    : QObject(options)
    , options(options)
    , optionName(std::move(name))
    , currentValue(std::move(value))
    , optionTypeId(typeId)
{
}

void MLOptionObject::setValue(const QVariant& value)
{
    if (value.userType() != optionTypeId) {
        qFatal("MatterLeast option '%s' was assigned a value of a different type",
               qPrintable(optionName));
    }
    if (currentValue == value) {
        return;
    }

    currentValue = value;
    options->persistValue(optionName, value);
    emit changed(value);
}

MLOptions::MLOptions() = default;

MLOptions* MLOptions::instance()
{
    static MLOptions options;
    return &options;
}

QVariant MLOptions::value(const QString& name,
                          const QVariant& defaultValue) const
{
    const int requestedType = defaultValue.userType();
    if (auto* existing = optionObjects.value(name, nullptr)) {
        ensureOptionType(name, existing, requestedType);
        return existing->value();
    }
    return settings.value(name, defaultValue);
}

void MLOptions::setValue(const QString& name, const QVariant& value)
{
    const int requestedType = value.userType();
    if (auto* existing = optionObjects.value(name, nullptr)) {
        ensureOptionType(name, existing, requestedType);
        existing->setValue(value);
        return;
    }
    persistValue(name, value);
}

bool MLOptions::contains(const QString& name) const
{
    return settings.contains(name);
}

MLOptionObject* MLOptions::optionObject(const QString& name,
                                        const QVariant& defaultValue)
{
    const int requestedType = defaultValue.userType();
    if (auto* existing = optionObjects.value(name, nullptr)) {
        ensureOptionType(name, existing, requestedType);
        return existing;
    }

    auto* option = new MLOptionObject(
        this, name, value(name, defaultValue), requestedType);
    optionObjects.insert(name, option);
    return option;
}

void MLOptions::ensureOptionType(const QString& name,
                                 const MLOptionObject* option,
                                 int requestedType)
{
    if (option->typeId() != requestedType) {
        qFatal("MatterLeast option '%s' was requested with a different type",
               qPrintable(name));
    }
}

void MLOptions::persistValue(const QString& name, const QVariant& value)
{
    settings.setValue(name, value);
    settings.sync();
}

} // namespace Mattermost
