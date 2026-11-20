include_guard(GLOBAL)

# OpenSceneGraph：只用于读取倾斜摄影（.osgb）数据，不参与渲染。
# 预编译树放在 third_party/osg（include/lib/bin/plugins），缺失时跳过倾斜摄影插件。
set(VIEW3D_OSG_ROOT "${CMAKE_SOURCE_DIR}/third_party/osg" CACHE PATH
    "OpenSceneGraph 预编译树根目录")
set(VIEW3D_OSG_FOUND FALSE)

set(_osg_node_header "${VIEW3D_OSG_ROOT}/include/osg/Node")
set(_osg_main_lib "${VIEW3D_OSG_ROOT}/lib/osg.lib")
if(EXISTS "${_osg_node_header}" AND EXISTS "${_osg_main_lib}")
    file(GLOB _osg_plugin_dirs LIST_DIRECTORIES true "${VIEW3D_OSG_ROOT}/plugins/osgPlugins-*")
    list(SORT _osg_plugin_dirs)
    list(GET _osg_plugin_dirs -1 VIEW3D_OSG_PLUGIN_DIR)
    set(VIEW3D_OSG_BIN_DIR "${VIEW3D_OSG_ROOT}/bin")
    set(VIEW3D_OSG_FOUND TRUE)
    message(STATUS "View3D: 找到 OpenSceneGraph（${VIEW3D_OSG_ROOT}）")
else()
    message(STATUS "View3D: 未找到 OpenSceneGraph（${VIEW3D_OSG_ROOT}），跳过倾斜摄影插件")
endif()

if(NOT VIEW3D_OSG_FOUND)
    return()
endif()

function(view3d_add_osg_imported target_name import_lib)
    add_library(${target_name} SHARED IMPORTED GLOBAL)
    set_target_properties(${target_name} PROPERTIES
        IMPORTED_LOCATION "${VIEW3D_OSG_BIN_DIR}/${import_lib}.dll"
        IMPORTED_IMPLIB "${VIEW3D_OSG_ROOT}/lib/${import_lib}.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${VIEW3D_OSG_ROOT}/include")
endfunction()

view3d_add_osg_imported(View3D::Osg osg)
view3d_add_osg_imported(View3D::OsgDB osgDB)
view3d_add_osg_imported(View3D::OsgUtil osgUtil)
view3d_add_osg_imported(View3D::OpenThreads OpenThreads)
set_property(TARGET View3D::OsgDB APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    View3D::Osg View3D::OpenThreads)
# osgUtil 的遍历器（ShaderGen/TriFun/SmoothVisitor）被模型读取器用来把任意节点图
# 压平成网格，需要同时看到 osgDB（读文件）与 osg。
set_property(TARGET View3D::OsgUtil APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    View3D::Osg View3D::OpenThreads)

# 运行时依赖：third_party/osg 现在是全量插件树（81 个 osgDB 插件 + 全部 nodekit 序列化
# 器，含 gdal/shp/ogr 之外的所有格式），逐个列名字维护必然漏，因此整目录部署：
# bin/*.dll 与 plugins/osgPlugins-<版本>/*.dll 都进应用 bin，插件目录与 osgDB.dll 同级
# （OSG 标准布局；放在 plugins/ 子目录里换机器就会加载失败）。
# osgdb_gdal/shp/ogr 在入库时已剔除，它们要拖 gdal+proj+sqlite+curl 约 60 MB 闭包。
file(GLOB _view3d_osg_runtime_dll_paths LIST_DIRECTORIES false "${VIEW3D_OSG_BIN_DIR}/*.dll")
file(GLOB _view3d_osg_plugin_dll_paths LIST_DIRECTORIES false "${VIEW3D_OSG_PLUGIN_DIR}/*.dll")
foreach(_path ${_view3d_osg_runtime_dll_paths})
    get_filename_component(_name "${_path}" NAME)
    list(APPEND VIEW3D_OSG_RUNTIME_DLLS "${_name}")
endforeach()
foreach(_path ${_view3d_osg_plugin_dll_paths})
    get_filename_component(_name "${_path}" NAME)
    list(APPEND VIEW3D_OSG_PLUGIN_DLLS "${_name}")
endforeach()
