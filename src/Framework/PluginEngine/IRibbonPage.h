#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IRibbonPage : public IPlugin {
public:
    virtual ~IRibbonPage() = default;

    virtual QString getCategoryKey() const = 0;
    virtual QString getName() const = 0;
    virtual QString getText() const = 0;
    virtual bool getSelected() const = 0;
};

} // namespace PluginEngine
} // namespace RF
