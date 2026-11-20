find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui)

# 网格数据契约：不依赖 GUI，数据插件可在工作线程或离线工具中使用。
add_library(View3DMeshContract STATIC
    ModelMesh.cpp
    ModelMesh.h)
add_library(View3D::MeshContract ALIAS View3DMeshContract)
set_target_properties(View3DMeshContract PROPERTIES FOLDER "View3D/RenderContract")
target_include_directories(View3DMeshContract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(View3DMeshContract PUBLIC Qt6::Core)
target_compile_definitions(View3DMeshContract PUBLIC
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
if(MSVC)
    target_compile_options(View3DMeshContract PRIVATE /W4 /permissive- /utf-8)
endif()

# 渲染契约：场景/视口能力接口与共享类型。纯头文件，不依赖插件引擎，
# 也不依赖 QWidget 实现；模型网格的实现符号由 View3DMeshContract 提供。
add_library(View3DRenderContract INTERFACE
    CameraTypes.h
    GlobeMath.h
    IRenderScene.h
    IRenderService.h
    IRenderSurface.h
    ISceneContent.h
    ISceneDisplay.h
    IGeoReference.h
    ICrsLibrary.h
    ICrsBrowser.h
    IImageryService.h
    IOsbLoader.h
    ITilesetLoader.h
    IViewportCamera.h
    IViewportCapture.h
    IViewportDisplay.h
    IViewportLayout.h
    IViewportOverlay.h
    IViewportPicking.h
    RenderTypes.h
    TileCacheSettings.h
    View3DObjectIds.h
    ViewportPickResult.h)
add_library(View3D::RenderContract ALIAS View3DRenderContract)
set_target_properties(View3DRenderContract PROPERTIES FOLDER "View3D/RenderContract")
target_include_directories(View3DRenderContract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(View3DRenderContract INTERFACE
    View3D::MeshContract Qt6::Core Qt6::Gui)
target_compile_definitions(View3DRenderContract INTERFACE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
