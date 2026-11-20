find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)

# 地理能力插件：影像底图、坐标定位+FlyTo、地理参考。
# 复用 SceneView 的图层树与渲染场景；地形/DEM 统一走模型导入插件的栅格读取器，
# 要素属性表在 plugin.view3d.features。
add_view3d_plugin(View3DGeoPlugin plugin.view3d.geo)

target_sources(View3DGeoPlugin PRIVATE
    GeoPluginEntry.cpp
    GeoImageryService.cpp
    GeoNavigationCommand.cpp
    GeoImageryService.h
    GeoNavigationCommand.h)

target_link_libraries(View3DGeoPlugin PRIVATE
    View3D::Contract Qt6::Widgets Qt6::Network)
