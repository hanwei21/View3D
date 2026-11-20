find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 倾斜摄影数据插件：只用 OpenSceneGraph 读取 .osgb 瓦片，渲染完全交给 Renderer 插件，
# 图层通过 ISceneLayerService 登记，因此目录树插件不依赖本插件。
add_view3d_plugin(View3DOsbPlugin plugin.view3d.osgb)
target_sources(View3DOsbPlugin PRIVATE
    OsgbGeometry.cpp OsgbGeometry.h
    OsgbMetadata.cpp OsgbMetadata.h
    OsgbPluginEntry.cpp
    OsgbRuntime.cpp OsgbRuntime.h
    OsgbStreaming.cpp OsgbStreaming.h)
target_link_libraries(View3DOsbPlugin PRIVATE
    View3D::Contract View3D::Osg View3D::OsgDB Qt6::Widgets)

# 运行库部署：OSG 核心 DLL 与 osgDB 插件都放 bin；插件目录 osgPlugins-<版本> 必须与
# osgDB.dll **同级**（OSG 的标准布局），塞进 plugins/ 子目录只有我们自己登记路径时能用，
# 换机器/别家 osgDB 就找不到。
get_filename_component(VIEW3D_OSG_PLUGIN_DIR_NAME "${VIEW3D_OSG_PLUGIN_DIR}" NAME)
set(_view3d_bin_dir "${CMAKE_BINARY_DIR}/bin")
set(_view3d_osg_plugin_dir "${_view3d_bin_dir}/${VIEW3D_OSG_PLUGIN_DIR_NAME}")

foreach(_osg_runtime_dll ${VIEW3D_OSG_RUNTIME_DLLS})
    add_custom_command(TARGET View3DOsbPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${VIEW3D_OSG_BIN_DIR}/${_osg_runtime_dll}" "${_view3d_bin_dir}"
        VERBATIM)
endforeach()

add_custom_command(TARGET View3DOsbPlugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_view3d_osg_plugin_dir}"
    VERBATIM)
foreach(_osg_plugin_dll ${VIEW3D_OSG_PLUGIN_DLLS})
    add_custom_command(TARGET View3DOsbPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${VIEW3D_OSG_PLUGIN_DIR}/${_osg_plugin_dll}" "${_view3d_osg_plugin_dir}"
        VERBATIM)
endforeach()
