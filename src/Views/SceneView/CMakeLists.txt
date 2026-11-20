find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 场景契约：图层、场景树、视图布局和 UI 命令；渲染能力来自 View3DRenderContract。
add_library(View3DContract INTERFACE)
add_library(View3D::Contract ALIAS View3DContract)
target_include_directories(View3DContract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(View3DContract INTERFACE
    View3D::RenderContract View3D::PluginEngine
    Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(View3DContract INTERFACE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_sources(View3DContract INTERFACE
    CommandStatePublisher.h
    CoreTypes.h
    IDataOpen.h
    ILayer.h
    ILayerManager.h
    ILayerPropertiesDock.h
    ISceneLayerService.h
    ISceneTreeDock.h
    ISceneTreeMenu.h
    IViewCommandService.h
    IViewportMenu.h)

# 场景工作区插件：不链接 Ogre，只通过 IRenderService/IRenderSurface 使用渲染能力。
add_view3d_plugin(View3DSceneViewPlugin plugin.view3d.sceneview)
target_sources(View3DSceneViewPlugin PRIVATE
    SceneViewPluginEntry.cpp
    GeoLayer.cpp GeoLayer.h
    LayerGroup.cpp LayerGroup.h
    LayerPropertiesDock.cpp LayerPropertiesDock.h
    LayerManager.cpp LayerManager.h
    ModelLayer.cpp ModelLayer.h
    SceneLayerService.cpp SceneLayerService.h
    SceneTreeCoordinator.cpp SceneTreeCoordinator.h
    SceneTreeDock.cpp SceneTreeDock.h
    SystemConfig.cpp SystemConfig.h
    ViewExtension.cpp ViewExtension.h
    ViewportContainer.cpp ViewportContainer.h
    ViewportContentsView.cpp)
target_link_libraries(View3DSceneViewPlugin PRIVATE
    View3DContract Qt6::Widgets)
