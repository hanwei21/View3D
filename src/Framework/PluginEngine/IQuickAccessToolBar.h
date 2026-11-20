#pragma once

#include "IBar.h"
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IQuickAccessToolBar : public IBar, public IPlugin {
public:
    virtual ~IQuickAccessToolBar() = default;
};

} // namespace PluginEngine
} // namespace RF
