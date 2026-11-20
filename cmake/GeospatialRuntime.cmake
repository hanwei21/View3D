include_guard(GLOBAL)

# GDAL 3.x + PROJ 9.8：栅格（GeoTIFF/DEM）与矢量（SHP/KML/GPKG）数据读取。
# 预编译树放在 third_party/geospatial（include/lib/bin/share），只按 C API 链 gdal.lib，
# PROJ/TIFF 等依赖留在 gdal.dll 的依赖树里，避免和工程内其他静态库符号冲突。
# 缺失时模型导入插件不编译栅格/矢量读取器。
set(VIEW3D_GEOSPATIAL_ROOT "${CMAKE_SOURCE_DIR}/third_party/geospatial" CACHE PATH
    "GDAL 预编译树根目录")
set(VIEW3D_GEOSPATIAL_FOUND FALSE)
set(VIEW3D_PROJ_FOUND FALSE)
set(VIEW3D_GEOSPATIAL_PROJ_DATA_DIR "${VIEW3D_GEOSPATIAL_ROOT}/share/proj")

# PROJ 单独作为引用目标：参照系插件只做坐标变换，按 proj.h 的 C API 链 proj_9.dll，
# 不依赖 GDAL；GDAL 缺失时重投影仍然可用。
if(EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/include/proj.h"
   AND EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/lib/proj.lib"
   AND EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/bin/proj_9.dll")
    add_library(View3D::Proj SHARED IMPORTED GLOBAL)
    set_target_properties(View3D::Proj PROPERTIES
        IMPORTED_LOCATION "${VIEW3D_GEOSPATIAL_ROOT}/bin/proj_9.dll"
        IMPORTED_IMPLIB "${VIEW3D_GEOSPATIAL_ROOT}/lib/proj.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${VIEW3D_GEOSPATIAL_ROOT}/include")
    set(VIEW3D_PROJ_FOUND TRUE)
    message(STATUS "View3D: 找到 PROJ（参照系重投影可用）")
else()
    message(STATUS "View3D: 未找到 PROJ，参照系退化为 EPSG:4326/3857 内置公式")
endif()

if(NOT EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/include/gdal.h"
   OR NOT EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/lib/gdal.lib"
   OR NOT EXISTS "${VIEW3D_GEOSPATIAL_ROOT}/bin/gdal.dll")
    message(STATUS "View3D: 未找到 GDAL（${VIEW3D_GEOSPATIAL_ROOT}），模型导入插件不含栅格/矢量读取器")
    return()
endif()

add_library(View3D::Gdal SHARED IMPORTED GLOBAL)
set_target_properties(View3D::Gdal PROPERTIES
    IMPORTED_LOCATION "${VIEW3D_GEOSPATIAL_ROOT}/bin/gdal.dll"
    IMPORTED_IMPLIB "${VIEW3D_GEOSPATIAL_ROOT}/lib/gdal.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${VIEW3D_GEOSPATIAL_ROOT}/include")

# 运行时部署：gdal.dll 的整条依赖（proj/tiff/curl/sqlite…）都要进应用 bin；
# GDAL 与 PROJ 的数据库文件按读取器里的约定部署到 bin/gdal-data 与 bin/proj-data。
file(GLOB VIEW3D_GEOSPATIAL_RUNTIME_DLLS LIST_DIRECTORIES false
    "${VIEW3D_GEOSPATIAL_ROOT}/bin/*.dll")
set(VIEW3D_GEOSPATIAL_GDAL_DATA_DIR "${VIEW3D_GEOSPATIAL_ROOT}/share/gdal")
set(VIEW3D_GEOSPATIAL_PROJ_DATA_DIR "${VIEW3D_GEOSPATIAL_ROOT}/share/proj")

list(LENGTH VIEW3D_GEOSPATIAL_RUNTIME_DLLS _geospatial_dll_count)
set(VIEW3D_GEOSPATIAL_FOUND TRUE)
message(STATUS "View3D: 找到 GDAL（${VIEW3D_GEOSPATIAL_ROOT}，运行库 ${_geospatial_dll_count} 个）")
