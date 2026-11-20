#pragma once

#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IWelcomeScreen : public IPlugin {
public:
    virtual ~IWelcomeScreen() = default;

    virtual int getResult() const = 0;

    virtual void close() = 0;
    virtual void show() = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF

