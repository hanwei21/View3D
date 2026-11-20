cmake_minimum_required(VERSION 3.25)

project(View3D VERSION 0.1.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CTest)
include(cmake/AddView3DPlugin.cmake)
include(cmake/OgreRuntime.cmake)
include(cmake/OsgRuntime.cmake)
include(cmake/CesiumNativeRuntime.cmake)
include(cmake/AssimpRuntime.cmake)
include(cmake/PointCloudRuntime.cmake)
include(cmake/GeospatialRuntime.cmake)

# 框架、渲染契约、渲染插件、场景插件、宿主外壳与应用程序始终一起构建。
add_subdirectory(src/Framework/PluginEngine)
add_subdirectory(src/RenderContract)
add_subdirectory(src/Views/Renderer)
add_subdirectory(src/Views/SceneView)
add_subdirectory(src/Views/ToolsPlugin)
# 模型导入插件：assimp/OSG/Cesium/点云/GDAL 各自独立可选，缺哪个就不编译哪个读取器。
add_subdirectory(src/Views/ModelPlugin)
# 参照系插件：主坐标系与源参照系重投影（PROJ 内核），只发布 IGeoReference。
add_subdirectory(src/Views/CrsPlugin)
# 地理能力插件：影像底图、坐标定位+FlyTo。地形/DEM 统一走模型导入插件的栅格读取器。
add_subdirectory(src/Views/GeoPlugin)
# 要素属性插件：拾取监听与属性表 dock，属性取自网格要素表，跨数据源通用。
add_subdirectory(src/Views/FeaturePlugin)
# 倾斜摄影插件依赖 SceneView 暴露的图层契约头文件，且需要本地 OSG 预编译树。
if(VIEW3D_OSG_FOUND)
    add_subdirectory(src/Views/OsgPlugin)
endif()
# 3D Tiles 插件需要本地 Cesium Native 预编译树，缺失时跳过。
if(VIEW3D_CESIUM_FOUND)
    add_subdirectory(src/Views/TilesetPlugin)
endif()
add_subdirectory(src/Framework/ApplicationPlugin)

add_custom_target(View3DFrameworkLibraries
    DEPENDS View3DPluginEngine View3DApplicationPlugin)
set_target_properties(View3DFrameworkLibraries PROPERTIES FOLDER "View3D/Framework")

add_subdirectory(src/App/View3D)

# 纯函数回归测试：不依赖渲染上下文，随 CTest 一起跑。
if(BUILD_TESTING)
    add_subdirectory(tests/math)
endif()

# Windows 安装包（CPack + NSIS）：打包已部署的 bin 运行期树，见 cmake/Packaging.cmake。
include(cmake/Packaging.cmake)
