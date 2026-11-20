#pragma once

#include <QMap>
#include <QString>
#include <functional>

namespace RF {
namespace PluginEngine {

class IApplication;
class IPluginContainer;
class IPlugin;

struct PluginCreationContext {
    IApplication* application = nullptr;
    IPluginContainer* container = nullptr;
    QString logicalType;
    QString pluginId;
    QString factoryId;
    QMap<QString, QString> attributes;
};

using PluginCreator = std::function<IPlugin*(const PluginCreationContext&)>;

} // namespace PluginEngine
} // namespace RF
