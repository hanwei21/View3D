find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# 模型导入插件：把「打开模型」这条流程从旧版数据预览搬进来。读取器按依赖分块编译，
# 缺哪个第三方库就不编哪个读取器，插件本身照常构建（对应类别的打开入口自动不出现）。
add_view3d_plugin(View3DModelPlugin plugin.view3d.model)

set(_model_sources
    ModelReadTypes.h
    ModelFormatRegistry.cpp
    ModelImportSession.cpp ModelImportSession.h
    ModelOpenFlow.cpp ModelOpenFlow.h
    ModelPluginEntry.cpp)

target_compile_features(View3DModelPlugin PRIVATE cxx_std_20)
target_compile_options(View3DModelPlugin PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus /bigobj /utf-8>)

set(_model_link View3D::Contract Qt6::Widgets)

if(VIEW3D_CESIUM_FOUND)
    list(APPEND _model_sources GltfModelImporter.cpp GltfModelImporter.h)
    list(APPEND _model_link View3D::Cesium)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_CESIUM)
endif()

if(VIEW3D_ASSIMP_FOUND)
    list(APPEND _model_sources
        AssimpModelImporter.cpp AssimpModelImporter.h
        ThreeDsKeyframeStream.cpp ThreeDsKeyframeStream.h)
    list(APPEND _model_link View3D::Assimp)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_ASSIMP)
endif()

if(VIEW3D_LASZIP_FOUND)
    list(APPEND _model_sources LasPointCloudImporter.cpp LasPointCloudImporter.h)
    list(APPEND _model_link View3D::Laszip)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_LAS)
endif()

if(VIEW3D_E57_FOUND)
    list(APPEND _model_sources E57PointCloudImporter.cpp E57PointCloudImporter.h)
    list(APPEND _model_link View3D::E57Format)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_E57)
endif()

if(VIEW3D_GEOSPATIAL_FOUND)
    list(APPEND _model_sources
        GdalRasterImporter.cpp GdalRasterImporter.h
        GdalVectorImporter.cpp GdalVectorImporter.h
        GeoReproject.cpp GeoReproject.h)
    list(APPEND _model_link View3D::Gdal)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_GDAL)
endif()

if(VIEW3D_OSG_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/OsgModelImporter.h")
    list(APPEND _model_sources OsgModelImporter.cpp OsgModelImporter.h)
    list(APPEND _model_link View3D::Osg View3D::OsgDB View3D::OsgUtil)
    target_compile_definitions(View3DModelPlugin PRIVATE VIEW3D_MODEL_HAS_OSG)
endif()

target_sources(View3DModelPlugin PRIVATE ${_model_sources})
target_link_libraries(View3DModelPlugin PRIVATE ${_model_link})

# ── 运行时部署 ────────────────────────────────────────────────────────────────
set(_model_bin_dir "${CMAKE_BINARY_DIR}/bin")

# GDAL 只链 gdal.lib，它自己的 PROJ/TIFF/CURL 依赖靠同目录的 DLL 解析；
# 读取器按 bin/gdal-data 与 bin/proj-data 的约定查找数据文件。
if(VIEW3D_GEOSPATIAL_FOUND)
    add_custom_command(TARGET View3DModelPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${VIEW3D_GEOSPATIAL_RUNTIME_DLLS} "${_model_bin_dir}"
        COMMENT "部署 GDAL/PROJ 运行库"
        VERBATIM)
    add_custom_command(TARGET View3DModelPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${VIEW3D_GEOSPATIAL_GDAL_DATA_DIR}" "${_model_bin_dir}/gdal-data"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${VIEW3D_GEOSPATIAL_PROJ_DATA_DIR}" "${_model_bin_dir}/proj-data"
        COMMENT "部署 GDAL/PROJ 数据"
        VERBATIM)
endif()

# OSG 运行库不在这里部署：根 CMakeLists 里倾斜摄影插件的加入条件与本插件编译
# OsgModelImporter 的条件同为 VIEW3D_OSG_FOUND，由它把核心 DLL 与 osgPlugins-<版本>
# 一次性放到 bin（插件目录必须与 osgDB.dll 同级，塞进 bin/plugins 只有我们自己登记
# 路径时能用，换机器就找不到读取器）。
