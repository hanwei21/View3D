#pragma once

#include "IPlugin.h"

namespace RF::PluginEngine {

class IApplication;

class IExtension : public IPlugin {
  public:
    ~IExtension() override = default;
    virtual bool onCreate(IApplication *application) = 0;
    virtual void onDestroy() = 0;
};

} // namespace RF::PluginEngine
