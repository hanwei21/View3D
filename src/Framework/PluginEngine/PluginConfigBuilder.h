#pragma once

#include "LayoutConfig.h"
#include "PEGlobals.h"
#include "IPlugin.h"
#include "PluginFactoryRegistry.h"

#include <memory>
#include <vector>

namespace RF {
namespace PluginEngine {

class IApplication;
class IPluginContainer;

// 按 LayoutConfig 创建命令、停靠窗口与内容视图，并登记到 PluginContainer。
// onCreate 不在这里调用：需要等 UI 绑定后再初始化。
class PLUGIN_ENGINE_EXPORT PluginConfigBuilder {
public:
    struct Result {
        QStringList warnings;
        std::vector<std::unique_ptr<IPlugin>> objects;
    };

    static Result buildFromLayout(const View3D::LayoutConfig& config,
                                  IApplication* application,
                                  IPluginContainer* container,
                                  const PluginFactoryRegistry* registry);
};

} // namespace PluginEngine
} // namespace RF
