find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 基础工具插件：坐标/距离/折线/角度/面积测量与结果面板。只通过
# IViewportPicking/IViewportOverlay 驱动视口，不链接 Ogre。
add_view3d_plugin(View3DToolsPlugin plugin.view3d.tools)
target_sources(View3DToolsPlugin PRIVATE
    MeasureDock.cpp MeasureDock.h
    MeasureSession.cpp MeasureSession.h
    MeasurementMath.cpp MeasurementMath.h
    MeasurementTypes.h
    ToolsPluginEntry.cpp)
target_link_libraries(View3DToolsPlugin PRIVATE
    View3D::Contract Qt6::Widgets)
