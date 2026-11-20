#pragma once

#include <QString>

namespace RF::PluginEngine {

// 框架自身的服务 ID；业务服务 ID 放在各自的 Contract 中。
inline const QString PluginRegistryServiceId = QStringLiteral("plugin.registry");

} // namespace RF::PluginEngine
