#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IFloatingHost : public IPlugin {
public:
    virtual ~IFloatingHost() = default;

    virtual QString getName() const = 0;
    virtual QString getText() const = 0;
};

} // namespace PluginEngine
} // namespace RF
