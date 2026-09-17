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

void MLOptions::persistValue(const QString& name, const QVariant& value)
{
    settings.setValue(name, value);
    settings.sync();
}

} // namespace Mattermost
