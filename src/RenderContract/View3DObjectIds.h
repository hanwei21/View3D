#pragma once

#include <QString>

namespace View3D {
namespace ServiceIds {
inline const QString Renderer = QStringLiteral("view3d.service.renderer");
inline const QString Scene = QStringLiteral("view3d.service.scene");
inline const QString Viewport = QStringLiteral("view3d.service.viewport");
inline const QString ViewportLayout = QStringLiteral("view3d.ui.viewport_layout");
inline const QString ViewCommands = QStringLiteral("view3d.commands.view");
inline const QString SceneLayers = QStringLiteral("view3d.service.scene_layers");
inline const QString SceneTreeDock = QStringLiteral("view3d.ui.scene_tree_dock");
inline const QString LayerPropertiesDock = QStringLiteral("view3d.ui.layer_properties_dock");
inline const QString SceneTreeMenu = QStringLiteral("view3d.ui.scene_tree_menu");
inline const QString ViewportMenu = QStringLiteral("view3d.ui.viewport_menu");
inline const QString DataOpen = QStringLiteral("view3d.data.open");
inline const QString OsbLoader = QStringLiteral("view3d.data.osgb");
inline const QString TilesetLoader = QStringLiteral("view3d.data.tileset");
inline const QString TileCache = QStringLiteral("view3d.service.tile_cache");
inline const QString GeoReference = QStringLiteral("view3d.service.geo_reference");
inline const QString CrsLibrary = QStringLiteral("view3d.service.crs_library");
// 内置参照系浏览入口（PROJ 权威库）：面板的左树与详情走它。
inline const QString CrsBrowser = QStringLiteral("view3d.service.crs_browser");
inline const QString Imagery = QStringLiteral("view3d.data.imagery");
}

namespace MessageTopics {
inline const QString RefreshCommandStates = QStringLiteral("ui.refresh_command_states");
// 参照系列发布后广播一次：面板与下拉可能比插件扩展先建好，只能靠这条消息补绑定。
inline const QString CrsLibraryReady = QStringLiteral("crs.library_ready");
}
} // namespace View3D
