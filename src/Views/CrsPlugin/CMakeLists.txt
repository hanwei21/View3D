find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 参照系插件：主坐标系（WGS84 经纬+椭球高 ↔ 场景局部米制）、世界原点首钉、
# 源参照系重投影（PROJ 内核，缺失时退化为 EPSG:4326/3857 内置公式）。
# 服务本身不消费任何东西，因此可最早创建，供影像/导航/导入器/tileset 领用；
# 「显示参照系」下拉是它唯一的界面，自定义 EPSG/WKT 需要输入框，所以链 Qt::Widgets。
add_view3d_plugin(View3DCrsPlugin plugin.view3d.crs)

target_sources(View3DCrsPlugin PRIVATE
    CrsPluginEntry.cpp
    CrsBackend.cpp CrsBackend.h
    CrsReference.cpp CrsReference.h
    CrsLibrary.cpp CrsLibrary.h
    CrsAuthority.cpp CrsAuthority.h
    CrsBrowserWidget.cpp CrsBrowserWidget.h
    CrsEditorDialog.cpp CrsEditorDialog.h
    CrsSelectDialog.cpp CrsSelectDialog.h
    CrsManagerDock.cpp CrsManagerDock.h)

target_link_libraries(View3DCrsPlugin PRIVATE
    View3D::RenderContract Qt6::Core Qt6::Gui Qt6::Widgets)

if(TARGET View3D::Proj)
    target_link_libraries(View3DCrsPlugin PRIVATE View3D::Proj)
    target_compile_definitions(View3DCrsPlugin PRIVATE VIEW3D_CRS_HAS_PROJ)
endif()

# PROJ 运行时与数据目录：读取器按 bin/proj-data 约定查找。
# 与模型导入插件的目标路径一致，重复复制是幂等的；单独启用 crs 插件时也能自足。
if(VIEW3D_PROJ_FOUND)
    set(_crs_bin_dir "${CMAKE_BINARY_DIR}/bin")
    add_custom_command(TARGET View3DCrsPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${VIEW3D_GEOSPATIAL_ROOT}/bin/proj_9.dll" "${_crs_bin_dir}"
        COMMENT "部署 PROJ 运行库")
    if(EXISTS "${VIEW3D_GEOSPATIAL_PROJ_DATA_DIR}")
        add_custom_command(TARGET View3DCrsPlugin POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${VIEW3D_GEOSPATIAL_PROJ_DATA_DIR}" "${_crs_bin_dir}/proj-data"
            COMMENT "部署 PROJ 数据")
    endif()
endif()
