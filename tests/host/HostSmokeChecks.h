#pragma once

#include "LayoutConfig.h"

#include <QString>

class QApplication;
class SARibbonMainWindow;

namespace ads {
class CDockManager;
}

namespace RF::PluginEngine {
class IApplication;
}

namespace View3DApplication {

class ThemeController;
class PluginUiHost;

bool runHostSmokeChecks(ThemeController &theme, SARibbonMainWindow &window,
                        PluginUiHost &uiHost, ads::CDockManager *dockManager,
                        RF::PluginEngine::IApplication &application,
                        const QList<View3D::RibbonPageLayout> &activePages,
                        const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
                        const QList<View3D::DockLayoutEntry> &activeDocks,
                        const QList<View3D::ContentsViewLayoutEntry> &activeContentsViews,
                        const QString &configPath, const QString &settingsPath,
                        QString *error);

void startSmokeWait(QApplication &application, RF::PluginEngine::IApplication &runtime);

} // namespace View3DApplication
