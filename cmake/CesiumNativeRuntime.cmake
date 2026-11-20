include_guard(GLOBAL)

# Cesium Native 0.62.0：3D Tiles 1.0/1.1 的解析、遍历与 glTF 解码。
# 预编译树放在 third_party/cesium-native（include/lib），它依赖的 vcpkg
# Release 静态库放在 third_party/cesium-native-deps。都缺失时跳过 3D Tiles 插件。
#
# 这里不消费 vcpkg 导出的 CMake 包：那些 targets 同时引用 debug/lib 与
# release/lib，而工程只入库 Release，走 find_package 会在 configure 阶段
# 因为不存在的 debug 库中止。依赖闭包由本文件显式列出，顺序按叶子到根。
set(VIEW3D_CESIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/cesium-native" CACHE PATH
    "Cesium Native 预编译树根目录")
set(VIEW3D_CESIUM_DEPS_ROOT "${CMAKE_SOURCE_DIR}/third_party/cesium-native-deps" CACHE PATH
    "Cesium Native 依赖静态库根目录")
set(VIEW3D_CESIUM_FOUND FALSE)

if(NOT EXISTS "${VIEW3D_CESIUM_ROOT}/include/Cesium3DTilesSelection/Tileset.h" OR
   NOT EXISTS "${VIEW3D_CESIUM_ROOT}/lib/Cesium3DTilesSelection.lib" OR
   NOT EXISTS "${VIEW3D_CESIUM_DEPS_ROOT}/lib/draco.lib")
    message(STATUS "View3D: 未找到 Cesium Native（${VIEW3D_CESIUM_ROOT}），跳过 3D Tiles 插件")
    return()
endif()

set(_view3d_cesium_library_names
    CesiumUtility
    CesiumJsonReader
    CesiumJsonWriter
    CesiumImage
    CesiumGeometry
    CesiumGeospatial
    CesiumGltf
    CesiumGltfReader
    CesiumGltfContent
    CesiumAsync
    Cesium3DTiles
    Cesium3DTilesReader
    Cesium3DTilesContent
    CesiumQuantizedMeshTerrain
    CesiumRasterOverlays
    CesiumVectorData
    Cesium3DTilesSelection)

set(_view3d_cesium_libraries)
foreach(_name ${_view3d_cesium_library_names})
    if(EXISTS "${VIEW3D_CESIUM_ROOT}/lib/${_name}.lib")
        list(APPEND _view3d_cesium_libraries "${VIEW3D_CESIUM_ROOT}/lib/${_name}.lib")
    else()
        message(STATUS "View3D: Cesium Native 缺少 ${_name}.lib，跳过 3D Tiles 插件")
        return()
    endif()
endforeach()

# vcpkg Release 闭包：zlib-ng/spdlog/glm/ada/s2/draco/meshoptimizer/modp_b64/spz/
# Async++/sqlite3/tinyxml2/KTX/WebP/libturbojpeg/blend2d/asmjit/OpenSSL/abseil…
# 逐个列名字必然漏（s2 会带出 absl + OpenSSL，KTX 会带出 zstd/astcenc），整目录链接。
# MSVC 只为被引用的符号拉取 .obj，多余库不会进最终 DLL。
file(GLOB _view3d_cesium_dep_libraries LIST_DIRECTORIES false
    "${VIEW3D_CESIUM_DEPS_ROOT}/lib/*.lib")
list(SORT _view3d_cesium_dep_libraries)
list(REMOVE_ITEM _view3d_cesium_dep_libraries
    "${VIEW3D_CESIUM_DEPS_ROOT}/lib/libcurl.lib")

if(NOT _view3d_cesium_dep_libraries)
    message(STATUS "View3D: Cesium Native 依赖库目录为空，跳过 3D Tiles 插件")
    return()
endif()

add_library(View3D::Cesium INTERFACE IMPORTED GLOBAL)
target_include_directories(View3D::Cesium SYSTEM INTERFACE
    "${VIEW3D_CESIUM_ROOT}/include"
    "${VIEW3D_CESIUM_DEPS_ROOT}/include")
# 这些定义来自 vcpkg 导出 target 的 INTERFACE_COMPILE_DEFINITIONS。少了
# SPDLOG_COMPILED_LIB 时 spdlog 会按 header-only 展开并在 Windows 上引入
# windows.h，它的 OPAQUE/MASK 宏会把 CesiumGltf/Material.h 的字段名替换掉。
target_compile_definitions(View3D::Cesium INTERFACE
    SPDLOG_COMPILED_LIB
    SPDLOG_FMT_EXTERNAL
    LIBASYNC_STATIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
# 静态库之间存在相互引用，MSVC 没有 --start-group，重复一遍保证后出现的库
# 也能解析前面库中的符号。
target_link_libraries(View3D::Cesium INTERFACE
    ${_view3d_cesium_libraries}
    ${_view3d_cesium_dep_libraries}
    ${_view3d_cesium_libraries}
    crypt32 ws2_32 advapi32 userenv)

set(VIEW3D_CESIUM_FOUND TRUE)
message(STATUS "View3D: 找到 Cesium Native 0.62（${VIEW3D_CESIUM_ROOT}）")
