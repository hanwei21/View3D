include_guard(GLOBAL)

# 点云依赖：LASzip（LAS/LAZ 解算）与 E57Format（E57 扫描仪点云）。
# 预编译树放在 third_party/pointcloud（include/lib），三者都是静态库，
# E57Format 还需要随带的 Xerces-C；缺失时模型导入插件不编译点云读取器。
set(VIEW3D_POINTCLOUD_ROOT "${CMAKE_SOURCE_DIR}/third_party/pointcloud" CACHE PATH
    "LASzip/E57Format 预编译树根目录")
set(VIEW3D_POINTCLOUD_FOUND FALSE)

if(NOT EXISTS "${VIEW3D_POINTCLOUD_ROOT}/include/laszip/laszip_api.h"
   OR NOT EXISTS "${VIEW3D_POINTCLOUD_ROOT}/lib/laszip3.lib")
    message(STATUS "View3D: 未找到 LASzip（${VIEW3D_POINTCLOUD_ROOT}），模型导入插件不含 LAS/LAZ 读取器")
    return()
endif()

add_library(View3D::Laszip STATIC IMPORTED GLOBAL)
set_target_properties(View3D::Laszip PROPERTIES
    IMPORTED_LOCATION "${VIEW3D_POINTCLOUD_ROOT}/lib/laszip3.lib"
    # laszip_api.h 在未定义 LASZIP_API_VERSION 时用 <laszip_common.h> 引用同目录
    # 头文件，所以头搜索路径要指到 include/laszip 而不是 include。
    INTERFACE_INCLUDE_DIRECTORIES "${VIEW3D_POINTCLOUD_ROOT}/include/laszip")

set(VIEW3D_E57_FOUND FALSE)
if(EXISTS "${VIEW3D_POINTCLOUD_ROOT}/include/E57Format/E57SimpleReader.h"
   AND EXISTS "${VIEW3D_POINTCLOUD_ROOT}/lib/E57Format.lib")
    add_library(View3D::E57Format STATIC IMPORTED GLOBAL)
    set_target_properties(View3D::E57Format PROPERTIES
        IMPORTED_LOCATION "${VIEW3D_POINTCLOUD_ROOT}/lib/E57Format.lib"
        # 读取器按 <E57Format.h> 直接引用，头文件在 include/E57Format；它自己的头
        # 又按 <xercesc/...> 引用随带的 Xerces-C，所以 include 根目录也要在路径里。
        INTERFACE_INCLUDE_DIRECTORIES
            "${VIEW3D_POINTCLOUD_ROOT}/include/E57Format;${VIEW3D_POINTCLOUD_ROOT}/include")
    # E57Export.h 只在定义 E57FORMAT_STATIC_DEFINE 时才把导出宏清空；随带的
    # Xerces-C 头已经把 XERCES_STATIC_LIBRARY 写死在 autoconf 头里，不用再传。
    set_property(TARGET View3D::E57Format APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS E57FORMAT_STATIC_DEFINE)
    if(EXISTS "${VIEW3D_POINTCLOUD_ROOT}/lib/xerces-c_3.lib")
        set_property(TARGET View3D::E57Format APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            "${VIEW3D_POINTCLOUD_ROOT}/lib/xerces-c_3.lib")
    endif()
    set(VIEW3D_E57_FOUND TRUE)
else()
    message(STATUS "View3D: 未找到 E57Format，模型导入插件不含 E57 读取器")
endif()

set(VIEW3D_LASZIP_FOUND TRUE)
set(VIEW3D_POINTCLOUD_FOUND TRUE)
message(STATUS "View3D: 找到点云依赖（${VIEW3D_POINTCLOUD_ROOT}，E57=${VIEW3D_E57_FOUND}）")
