#include <QObject>

#include <matterleast/plugin/PluginInterface.h>

class SmokePlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "SmokePlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)

public:
    bool enable() override { return true; }
    bool disable() override { return true; }
};

#include "SmokePlugin.moc"
