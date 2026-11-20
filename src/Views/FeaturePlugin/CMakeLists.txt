find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 要素属性插件：拾取监听 + 属性表 dock。属性来自网格要素表（ModelMeshDto::featureTables），
# 因此二维矢量、模型、3D Tiles/BIM 共用一条通路，不依赖地理插件。
add_view3d_plugin(View3DFeaturePlugin plugin.view3d.features)

target_sources(View3DFeaturePlugin PRIVATE
    FeaturePluginEntry.cpp
    FeatureTableDock.cpp FeatureTableDock.h)

target_link_libraries(View3DFeaturePlugin PRIVATE
    View3D::Contract Qt6::Widgets)
