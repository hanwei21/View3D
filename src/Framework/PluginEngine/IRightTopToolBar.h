#pragma once

#include "IBar.h"
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IRightTopToolBar : public IBar, public IPlugin {
public:
    virtual ~IRightTopToolBar() = default;
};

} // namespace PluginEngine
} // namespace RF
