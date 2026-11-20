#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IRibbonPageCategory : public IPlugin {
public:
    virtual ~IRibbonPageCategory() = default;

    virtual QString getAttachContentsViewKey() const = 0;
    virtual QString getName() const = 0;
    virtual QString getText() const = 0;
    virtual bool getVisible() const = 0;
};

} // namespace PluginEngine
} // namespace RF
