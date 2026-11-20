#pragma once

namespace RF {
namespace PluginEngine {

class IApplication;

class IConnect {
public:
    virtual ~IConnect() = default;

    virtual void onConnection(IApplication* app) = 0;
    virtual void onDisconnection() = 0;
};

} // namespace PluginEngine
} // namespace RF
