find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 3D Tiles 数据插件：Cesium Native 负责 1.0/1.1 清单、LOD 遍历与 glTF 解码，
# 转换出的网格通过 RenderContract 的瓦片组进入 Renderer，场景树插件不依赖本插件。
add_view3d_plugin(View3DTilesPlugin plugin.view3d.tileset)
target_sources(View3DTilesPlugin PRIVATE
    TilesetAccessor.cpp TilesetAccessor.h
    TilesetGeometry.cpp TilesetGeometry.h
    TilesetPluginEntry.cpp
    TilesetStreaming.cpp TilesetStreaming.h)
# Cesium Native 0.62 的公开头文件使用 std::span；依赖库也按 C++20 编译。
target_compile_features(View3DTilesPlugin PRIVATE cxx_std_20)
target_compile_options(View3DTilesPlugin PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus /bigobj /utf-8>)
target_link_libraries(View3DTilesPlugin PRIVATE
    View3D::Contract View3D::Cesium Qt6::Widgets)
