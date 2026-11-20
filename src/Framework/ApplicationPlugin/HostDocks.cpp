#include "HostDocks.h"

#include "LogDock.h"
#include "ThemeController.h"

#include <PluginFactoryRegistry.h>

namespace View3DApplication {

void registerHostDockFactory(RF::PluginEngine::PluginFactoryRegistry& registry)
{
    registry.registerCreator(
        hostCommandPluginId(), QStringLiteral("factory.create_dockwindow"),
        QStringLiteral("dockwindow"),
        [](const RF::PluginEngine::PluginCreationContext& context)
            -> RF::PluginEngine::IPlugin* {
            const QString name = context.attributes.value(QStringLiteral("name"));
            if (name == QStringLiteral("dw.log"))
                return createLogDock(context);
            if (name == QStringLiteral("dw.plugins"))
                return createPluginManagerDock(context);
            return nullptr;
        });
}

} // namespace View3DApplication
