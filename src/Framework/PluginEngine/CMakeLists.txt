find_package(Qt6 6.8 REQUIRED COMPONENTS Core)

# 通用插件 / 状态 / 消息 / UI 对象注册表框架，不引用任何渲染或业务模块。
add_library(View3DPluginEngine SHARED
    PluginContainer.cpp
    PluginContainer.h
    IPluginContainer.h
    PluginCollection.cpp
    PluginCollection.h
    StatesManager.cpp
    StatesManager.h
    IStatesManager.h
    IServicePublisher.h
    IUiObjectRegistry.h
    PluginFactoryRegistry.cpp
    PluginFactoryRegistry.h
    IPlugin.h
    IExtension.h
    IMessageCenter.h
    MessageCenter.h
    MessageCenter.cpp
    IWorkSpace.h
    WorkSpace.h
    WorkSpace.cpp
    ICommand.h
    ICommandService.h
    ICommandStatePublisher.h
    IChoiceCommand.h
    IPluginObjectFactory.h
    PluginServiceIds.h
    View3DPluginEntry.h
    PEGlobals.h
    LayoutConfig.cpp
    LayoutConfig.h
    PluginConfigBuilder.cpp
    PluginConfigBuilder.h
    Application.cpp
    Application.h
    AppLog.h
    AppLogManager.cpp
    AppLogManager.h
    IAppLog.h
    IAppLogManager.h
    RecentFileManager.cpp
    RecentFileManager.h
    RecentFileGroup.h
    IRecentFileGroup.h
    IRecentFileManager.h
    DynamicMenuManager.cpp
    DynamicMenuManager.h
    IDynamicMenuManager.h
    CheckModule.cpp
    CheckModule.h
    PluginEngine.cpp
)
set_target_properties(View3DPluginEngine PROPERTIES FOLDER "View3D/Framework")
add_library(View3D::PluginEngine ALIAS View3DPluginEngine)
target_include_directories(View3DPluginEngine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/View3DPluginSDK>)
target_link_libraries(View3DPluginEngine PUBLIC Qt6::Core)
target_compile_definitions(View3DPluginEngine
    PRIVATE PLUGIN_ENGINE_BUILD
    PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
)
if(MSVC)
    target_compile_options(View3DPluginEngine PRIVATE /W4 /permissive- /utf-8)
endif()
