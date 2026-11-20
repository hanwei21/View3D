#pragma once

namespace RF {
namespace PluginEngine {

class IFunction {
public:
    virtual ~IFunction() = default;

    virtual void* invoke(void* obj, void* arg) = 0;
};

} // namespace PluginEngine
} // namespace RF
