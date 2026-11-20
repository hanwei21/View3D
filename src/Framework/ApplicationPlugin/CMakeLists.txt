find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Svg)
find_package(SARibbonBar 2.8 CONFIG REQUIRED)
find_package(qtadvanceddocking 4.0 CONFIG REQUIRED)

# 产品外壳：按 ui_config.xml 装配 SARibbonBar 与 ADS 停靠布局，不含业务知识。
add_library(View3DApplicationPlugin SHARED
    View3DApplication.cpp
    DockLayoutUtils.h
    FramelessEdgeFilter.h
    CrashDiagnostics.cpp CrashDiagnostics.h
    HostDocks.cpp HostDocks.h
    LogDock.cpp LogDock.h
    PluginUiHost.cpp PluginUiHost.h
    PluginManagerDock.cpp PluginManagerDock.h
    ThemeController.cpp ThemeController.h
)
target_include_directories(View3DApplicationPlugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(BUILD_TESTING)
    target_sources(View3DApplicationPlugin PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/host/HostSmokeChecks.cpp"
        "${CMAKE_SOURCE_DIR}/tests/host/HostSmokeChecks.h")
    target_include_directories(View3DApplicationPlugin PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/host")
    target_compile_definitions(View3DApplicationPlugin PRIVATE
        VIEW3D_HOST_SMOKE_CHECKS)
endif()
target_link_libraries(View3DApplicationPlugin PRIVATE
    View3DPluginEngine View3DContract Qt6::Widgets Qt6::Svg
    SARibbonBar::SARibbonBar ads::qtadvanceddocking dbghelp)
target_compile_definitions(View3DApplicationPlugin PRIVATE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
# 产品外壳属于宿主系统组件，与 View3DPluginEngine 同目录部署；它不是可插拔插件包，
# 不进入 bin/plugins，也不由 <packages> 声明。
set_target_properties(View3DApplicationPlugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "View3D/Framework/ApplicationPlugin")
if(MSVC)
    target_compile_options(View3DApplicationPlugin PRIVATE /W4 /permissive- /utf-8)
endif()
