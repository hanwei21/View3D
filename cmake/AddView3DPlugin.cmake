include_guard(GLOBAL)

# 插件 DLL 的公共构建规则：输出到 bin/plugins，链接插件框架与最小契约层。
function(add_view3d_plugin target_name plugin_id)
    add_library(${target_name} SHARED)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target_name} PRIVATE
        View3DPluginEngine Qt6::Core)
    target_compile_definitions(${target_name} PRIVATE
        VIEW3D_PLUGIN_BUILD
        QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
    set_target_properties(${target_name} PROPERTIES
        FOLDER "View3D/Plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()
