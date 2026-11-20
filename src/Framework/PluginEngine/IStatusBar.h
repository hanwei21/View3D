#pragma once

#include "IBar.h"
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IStatusBar : public IBar, public IPlugin {
public:
    virtual ~IStatusBar() = default;

    virtual bool getIsDefaultStatusBar() const = 0;
};

} // namespace PluginEngine
} // namespace RF
