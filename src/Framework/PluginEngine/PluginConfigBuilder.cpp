#include "PluginConfigBuilder.h"

#include <ICommand.h>
#include <IContentsView.h>
#include <IDockWindow.h>
#include <IPluginContainer.h>
#include <IPluginObjectFactory.h>

#include <QSet>

namespace RF {
namespace PluginEngine {
namespace {

void addCommandAttributes(PluginCreationContext& context,
                          const View3D::RibbonGroupLayout::Item& item)
{
    context.attributes = item.attributes;
    context.attributes.insert(QStringLiteral("name"), item.name);
    context.attributes.insert(QStringLiteral("caption"), item.caption);
    context.attributes.insert(QStringLiteral("hint"), item.hint);
    context.attributes.insert(QStringLiteral("icon"), item.icon);
    context.attributes.insert(QStringLiteral("iconSize"), item.iconSize);
}

void collectCommand(const View3D::RibbonGroupLayout::Item& item,
                    IApplication* application, IPluginContainer* container,
                    const PluginFactoryRegistry* registry, QSet<QString>* created,
                    PluginConfigBuilder::Result* result)
{
    if (item.name.isEmpty() || created->contains(item.name))
        return;

    PluginCreationContext context;
    context.application = application;
    context.container = container;
    context.logicalType = item.type;
    context.pluginId = item.pluginId;
    context.factoryId = item.factoryId;
    addCommandAttributes(context, item);

    std::unique_ptr<IPlugin> plugin(registry ? registry->create(context) : nullptr);
    auto* command = dynamic_cast<ICommand*>(plugin.get());
    if (!command) {
        result->warnings.append(QStringLiteral("命令工厂不可用：%1").arg(item.name));
        return;
    }
    container->getCommands().insert(item.name, command);
    created->insert(item.name);
    result->objects.push_back(std::move(plugin));
}

void collectDockWindow(const View3D::DockLayoutEntry& entry, IApplication* application,
                       IPluginContainer* container, const PluginFactoryRegistry* registry,
                       PluginConfigBuilder::Result* result)
{
    if (entry.name.isEmpty())
        return;

    PluginCreationContext context;
    context.application = application;
    context.container = container;
    context.logicalType = QStringLiteral("dockwindow");
    context.pluginId = entry.pluginId;
    context.factoryId = entry.factoryId;
    context.attributes = entry.attributes;
    context.attributes.insert(QStringLiteral("name"), entry.name);
    context.attributes.insert(QStringLiteral("text"), entry.text);
    context.attributes.insert(QStringLiteral("area"), entry.area);

    std::unique_ptr<IPlugin> plugin(registry ? registry->create(context) : nullptr);
    auto* dockWindow = dynamic_cast<IDockWindow*>(plugin.get());
    if (!dockWindow) {
        result->warnings.append(QStringLiteral("停靠窗口工厂不可用：%1").arg(entry.name));
        return;
    }
    container->getDockWindows().insert(entry.name, dockWindow);
    result->objects.push_back(std::move(plugin));
}

void collectContentsView(const View3D::ContentsViewLayoutEntry& entry,
                         IApplication* application, IPluginContainer* container,
                         const PluginFactoryRegistry* registry,
                         PluginConfigBuilder::Result* result)
{
    if (entry.name.isEmpty())
        return;

    PluginCreationContext context;
    context.application = application;
    context.container = container;
    context.logicalType = QStringLiteral("contentsview");
    context.pluginId = entry.pluginId;
    context.factoryId = entry.factoryId;
    context.attributes = entry.attributes;
    context.attributes.insert(QStringLiteral("name"), entry.name);
    context.attributes.insert(QStringLiteral("text"), entry.text);
    context.attributes.insert(QStringLiteral("defaultHost"), entry.defaultHost);

    std::unique_ptr<IPlugin> plugin(registry ? registry->create(context) : nullptr);
    auto* contentsView = dynamic_cast<IContentsView*>(plugin.get());
    if (!contentsView) {
        result->warnings.append(QStringLiteral("内容视图工厂不可用：%1").arg(entry.name));
        return;
    }
    container->getContentsViews().insert(entry.name, contentsView);
    result->objects.push_back(std::move(plugin));
}

} // namespace

PluginConfigBuilder::Result PluginConfigBuilder::buildFromLayout(
    const View3D::LayoutConfig& config, IApplication* application,
    IPluginContainer* container, const PluginFactoryRegistry* registry)
{
    Result result;
    if (!container) {
        result.warnings.append(QStringLiteral("PluginContainer is null"));
        return result;
    }

    QSet<QString> commands;
    for (const View3D::RibbonPageLayout& page : config.ribbonPages)
        for (const View3D::RibbonGroupLayout& group : page.groups)
            for (const View3D::RibbonGroupLayout::Item& item : group.items)
                collectCommand(item, application, container, registry, &commands, &result);
    for (const View3D::RibbonGroupLayout::Item& item : config.ribbonRightButtons)
        collectCommand(item, application, container, registry, &commands, &result);

    for (const View3D::DockLayoutEntry& entry : config.docks)
        collectDockWindow(entry, application, container, registry, &result);
    for (const View3D::ContentsViewLayoutEntry& entry : config.contentsViews)
        collectContentsView(entry, application, container, registry, &result);

    return result;
}

} // namespace PluginEngine
} // namespace RF
