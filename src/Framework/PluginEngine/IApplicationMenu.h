#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplicationMenu : public IPlugin {
public:
    virtual ~IApplicationMenu() = default;

    virtual QString getName() const = 0;
    virtual QString getCaption() const = 0;
    virtual QString getImage() const = 0;
};

} // namespace PluginEngine
} // namespace RF
