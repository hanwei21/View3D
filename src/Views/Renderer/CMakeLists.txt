find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

if(NOT DEFINED OGRE_NEXT_ROOT)
    message(FATAL_ERROR "OGRE_NEXT_ROOT must point to the Ogre-Next v3.0.0 install prefix")
endif()

# Ogre 渲染插件：唯一链接 Ogre 的插件，通过 RenderContract 提供视口表面。
add_view3d_plugin(View3DRendererPlugin plugin.view3d.renderer)
target_sources(View3DRendererPlugin PRIVATE
    RendererPluginEntry.cpp
    OgreRuntime.cpp OgreRuntime.h
    OgrePbsReflection.cpp OgrePbsReflection.h
    OgreScene.cpp OgreScene.h
    OgreViewportSurface.cpp OgreViewportSurface.h)
target_include_directories(View3DRendererPlugin PRIVATE
    "${OGRE_NEXT_ROOT}/include/OGRE"
    "${OGRE_NEXT_ROOT}/include/OGRE/Hlms/Common"
    "${OGRE_NEXT_ROOT}/include/OGRE/Hlms/Unlit"
    "${OGRE_NEXT_ROOT}/include/OGRE/Hlms/Pbs")
target_link_libraries(View3DRendererPlugin PRIVATE
    View3D::RenderContract Qt6::Widgets
    "${OGRE_NEXT_ROOT}/lib/Release/OgreMain.lib"
    "${OGRE_NEXT_ROOT}/lib/Release/OgreHlmsUnlit.lib"
    "${OGRE_NEXT_ROOT}/lib/Release/OgreHlmsPbs.lib"
    winmm)
target_compile_definitions(View3DRendererPlugin PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(View3DRendererPlugin PRIVATE /W4 /permissive- /utf-8)
endif()
