#pragma once

#include <QtPlugin>

namespace MatterLeast::PluginApi {

/**
 * Minimal lifecycle contract shared by every native MatterLeast plugin.
 * Capability/service APIs are deliberately separate interfaces so this base
 * IID can remain stable while individual plugin features evolve independently.
 */
class PluginInterface
{
public:
    virtual ~PluginInterface() = default;

    virtual bool enable() = 0;
    virtual bool disable() = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_PLUGIN_IID "io.github.Ri0n.MatterLeast.Plugin/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::PluginInterface, MATTERLEAST_PLUGIN_IID)
