#pragma once

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantHash>
#include <QtPlugin>

namespace MatterLeast::PluginApi {

/**
 * Optional declarative settings extension. The host renders and persists
 * settings; plugins never receive MatterLeast internal objects or QSettings.
 *
 * Each descriptor is a QVariantHash with:
 *   key: stable setting ID
 *   title: user-visible label
 *   type: "integer" or "boolean"
 *   default: value of that type
 * For "integer": minimum, maximum, step (integers), optional suffix.
 * Optional description supplies explanatory text below the control.
 *
 * setSettingValue() applies a validated value live and returns false if the
 * plugin rejects it. The host owns persistence and replays stored values on
 * each enable. Implementations must not persist values independently.
 */
class PluginSettingsInterface
{
public:
    virtual ~PluginSettingsInterface() = default;

    virtual QList<QVariantHash> settingDescriptors() const = 0;
    virtual bool setSettingValue(const QString& key, const QVariant& value) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_PLUGIN_SETTINGS_IID "io.github.Ri0n.MatterLeast.PluginSettings/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::PluginSettingsInterface,
                    MATTERLEAST_PLUGIN_SETTINGS_IID)
