find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Svg)
# imported target 只在 find_package 所在目录及子目录可见，部署步骤要单独声明。
find_package(SARibbonBar 2.8 CONFIG REQUIRED)
find_package(qtadvanceddocking 4.0 CONFIG REQUIRED)

add_executable(View3D WIN32 main.cpp LogTee.cpp LogTee.h View3D.rc)
set_target_properties(View3D PROPERTIES FOLDER "View3D/App")
target_link_libraries(View3D PRIVATE Qt6::Core)
add_dependencies(View3D View3DApplicationPlugin View3DSceneViewPlugin View3DRendererPlugin)
if(TARGET View3DTilesPlugin)
    add_dependencies(View3D View3DTilesPlugin)
endif()
target_compile_definitions(View3D PRIVATE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

set(VIEW3D_UI_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/ui_config.xml")

# ---- 运行期部署 -------------------------------------------------------------

get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _qt_core_dll)
    get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
endif()
if(_qt_core_dll)
    get_filename_component(_qt_bin_dir "${_qt_core_dll}" DIRECTORY)
    get_filename_component(_qt_prefix "${_qt_bin_dir}" DIRECTORY)
endif()

# 配置文件的复制不能只挂在 exe 的 POST_BUILD 上：只改 ui_config.xml 时
# exe 不会重链，单纯改插件也不会触发复制。用独立目标保证每次构建都检查。
add_custom_target(View3DRuntimeConfig ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:View3D>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${VIEW3D_UI_CONFIG}"
        "$<TARGET_FILE_DIR:View3D>/ui_config.xml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/system_config.xml"
        "$<TARGET_FILE_DIR:View3D>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:View3D>/resources"
    DEPENDS "${VIEW3D_UI_CONFIG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/system_config.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/globe_basemap.jpg"
    VERBATIM)
add_custom_command(TARGET View3DRuntimeConfig POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/config/ui_config.test.xml"
        "$<TARGET_FILE_DIR:View3D>/ui_config.test.xml"
    VERBATIM)
set_target_properties(View3DRuntimeConfig PROPERTIES FOLDER "View3D/Host")
add_dependencies(View3D View3DRuntimeConfig)

foreach(_ogre_dll ${OGRE_RUNTIME_DLLS})
    add_custom_command(TARGET View3D POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_ogre_dll}" "$<TARGET_FILE_DIR:View3D>"
        VERBATIM)
endforeach()

foreach(_hlms_dir ${OGRE_HLMS_MEDIA_DIRS})
    get_filename_component(_hlms_name "${_hlms_dir}" NAME)
    add_custom_command(TARGET View3D POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_hlms_dir}" "$<TARGET_FILE_DIR:View3D>/Hlms/${_hlms_name}"
        VERBATIM)
endforeach()

add_custom_command(TARGET View3D POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:SARibbonBar::SARibbonBar>"
        "$<TARGET_FILE:ads::qtadvanceddocking>"
        "$<TARGET_FILE_DIR:View3D>"
    VERBATIM)

if(_qt_bin_dir)
    foreach(_qt_module Qt6Core Qt6Gui Qt6Widgets Qt6Svg Qt6Network)
        if(EXISTS "${_qt_bin_dir}/${_qt_module}.dll")
            add_custom_command(TARGET View3D POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_qt_bin_dir}/${_qt_module}.dll" "$<TARGET_FILE_DIR:View3D>"
                VERBATIM)
        endif()
    endforeach()
    foreach(_plugin_dir platforms imageformats)
        if(EXISTS "${_qt_prefix}/plugins/${_plugin_dir}")
            add_custom_command(TARGET View3D POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_qt_prefix}/plugins/${_plugin_dir}"
                    "$<TARGET_FILE_DIR:View3D>/${_plugin_dir}"
                VERBATIM)
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    set(_view3d_test_env_mods
        "PATH=path_list_prepend:${_qt_bin_dir}"
        "PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/third_party/ui-deps/bin"
        "PATH=path_list_prepend:${OGRE_RUNTIME_DIR}")

    # 参照系列库指到 bin 下的测试专用文件：无头用例会真的写盘，不能碰部署的
    # crs_library.json，也不要把用户机器上的库改掉。
    add_test(NAME View3D.Startup COMMAND View3D --smoke-test
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.Startup PROPERTIES
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "VIEW3D_SMOKE_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_SMOKE_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_CRS_LIBRARY=${CMAKE_BINARY_DIR}/bin/crs_library_test.json"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    add_test(NAME View3D.UiChecks COMMAND View3D --smoke-test
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.UiChecks PROPERTIES
        TIMEOUT 30
        RUN_SERIAL TRUE
        PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_THEME_FAILED;VIEW3D_RIBBON_FAILED;VIEW3D_SCENE_TREE_FAILED;VIEW3D_VIEWPORT_LAYOUT_FAILED;VIEW3D_CONFIG_UI_FAILED;VIEW3D_ICONS_FAILED;VIEW3D_PLUGIN_MANAGER_FAILED;VIEW3D_VIEW_CULL_FAILED;VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED;VIEW3D_EDGE_GUARD_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_CRS_LIBRARY=${CMAKE_BINARY_DIR}/bin/crs_library_test.json"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    add_test(NAME View3D.ConfigDriven
        COMMAND View3D --smoke-test --ui-config=ui_config.test.xml
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.ConfigDriven PROPERTIES
        TIMEOUT 30
        RUN_SERIAL TRUE
        PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_THEME_FAILED;VIEW3D_RIBBON_FAILED;VIEW3D_SCENE_TREE_FAILED;VIEW3D_VIEWPORT_LAYOUT_FAILED;VIEW3D_CONFIG_UI_FAILED;VIEW3D_ICONS_FAILED;VIEW3D_PLUGIN_MANAGER_FAILED;VIEW3D_VIEW_CULL_FAILED;VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_OSBG_ROAM_FAILED;VIEW3D_TILES_SMOKE_FAILED;VIEW3D_EDGE_GUARD_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_CRS_LIBRARY=${CMAKE_BINARY_DIR}/bin/crs_library_test.json"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")

    # 倾斜摄影缓存漫游回归：需要本机数据集，未设置 VIEW3D_OSBG_DIR 时自动跳过。
    # 同一机位多圈对比驻留量，验证"有加载也有卸载"。
    add_test(NAME View3D.OsbRoam COMMAND View3D --smoke-test
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.OsbRoam PROPERTIES
        TIMEOUT 480
        RUN_SERIAL TRUE
        PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_OSBG_ROAM_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_OSBG_ROAM_S=300"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")

    # 3DS 大场景回归：Lib3DS 写的 keyframer 没有父子层级记录，Assimp 沿父链无上限
    # 递归会把导入线程的 1 MB 栈打爆（进程静默闪退）。没有这份成果时不注册本用例。
    set(_view3d_3ds_large "E:/data/LJG 厂区整体三维模型3ds/LJG 厂区整体三维模型3ds.3ds")
    if(EXISTS "${_view3d_3ds_large}")
        add_test(NAME View3D.ThreeDsLarge COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.ThreeDsLarge PROPERTIES
            TIMEOUT 900
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_3DS_DATASET_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_3DS_DATASET_FAILED;VIEW3D_MODEL_IMPORT_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_3DS_FILE=${_view3d_3ds_large}"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()

    # 3D Tiles 1.0/1.1：使用本机 3d-tiles-samples 夹具，未安装数据时用例自动跳过。
    set(_view3d_tiles_10 "E:/data/tiles3d/3d-tiles-samples-main/1.0/TilesetWithDiscreteLOD/tileset.json")
    set(_view3d_tiles_11 "E:/data/tiles3d/3d-tiles-samples-main/1.1/MultipleContents/tileset.json")
    if(TARGET View3DTilesPlugin AND EXISTS "${_view3d_tiles_10}")
        add_test(NAME View3D.Tiles10 COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.Tiles10 PROPERTIES
            TIMEOUT 120
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_TILES_PICK_STRICT=1;VIEW3D_TILES_PATH=${_view3d_tiles_10}"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()
    if(TARGET View3DTilesPlugin AND EXISTS "${_view3d_tiles_11}")
        add_test(NAME View3D.Tiles11 COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.Tiles11 PROPERTIES
            TIMEOUT 120
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_TILES_PICK_STRICT=1;VIEW3D_TILES_PATH=${_view3d_tiles_11}"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()
    # 卸载 A 再加载 B：B 必须使用新的世界系并立即复位相机枢轴。
    if(TARGET View3DTilesPlugin AND EXISTS "${_view3d_tiles_10}" AND EXISTS "${_view3d_tiles_11}")
        add_test(NAME View3D.TilesSequence COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.TilesSequence PROPERTIES
            TIMEOUT 180
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_TILES_SEQUENCE_LOCAL=1;VIEW3D_TILES_PATH=${_view3d_tiles_10};VIEW3D_TILES_PATH2=${_view3d_tiles_11}"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()

    # 球面模式：同一份 ECEF 数据的进入/退出往返（相机局部↔ECEF 换算、
    # 会话重启后按球面重新烘焙上屏、非瓦片图层隐藏/恢复、椭球底面出图）。
    if(TARGET View3DTilesPlugin AND EXISTS "${_view3d_tiles_10}")
        add_test(NAME View3D.TilesGlobe COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.TilesGlobe PROPERTIES
            TIMEOUT 240
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_TILES_GLOBE_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_TILES_PICK_STRICT=1;VIEW3D_TILES_PATH=${_view3d_tiles_10};VIEW3D_TILES_GLOBE=1"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()

    # 倾斜摄影球面显示：按 metadata 的 SRS 逐顶点烘焙到 ECEF，中心视线拾取点
    # 的经纬度必须落在 SRSOrigin 附近；需要本机数据集，未安装时用例自动跳过。
    set(_view3d_osgb_globe "E:/data/osgb/OSGB-大雁塔")
    if(TARGET View3DOsbPlugin AND EXISTS "${_view3d_osgb_globe}/metadata.xml")
        add_test(NAME View3D.OsbGlobe COMMAND View3D --smoke-test
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(View3D.OsbGlobe PROPERTIES
            TIMEOUT 600
            RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "VIEW3D_OSGB_GLOBE_OK"
            FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED;VIEW3D_TILES_SMOKE_FAILED;VIEW3D_MODEL_GLOBE_FAILED"
            ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_OSBG_DIR=${_view3d_osgb_globe};VIEW3D_OSBG_GLOBE=1;VIEW3D_OSBG_ALLOW_SHALLOW=1"
            ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
    endif()

    # 无坐标系模型的球面路径：临时 OBJ 导入 → 自动重烘焙到 (0°,0°) 地表 → 退出恢复。
    add_test(NAME View3D.ModelGlobe COMMAND View3D --smoke-test
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.ModelGlobe PROPERTIES
        TIMEOUT 120
        RUN_SERIAL TRUE
        PASS_REGULAR_EXPRESSION "VIEW3D_MODEL_GLOBE_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_MODEL_GLOBE_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_MODEL_GLOBE=1"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")

    # 覆盖扫描：验收"任意视角下 footprint 内不得出现沿瓦片轮廓的空白"。
    # 绕数据集 12 个方位、轮换三档距离，逐视角按网格采样背景色占比。
    # 需要本机数据集，未设置 VIEW3D_OSBG_DIR 时自动跳过。
    add_test(NAME View3D.OsbCoverage COMMAND View3D --smoke-test
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set_tests_properties(View3D.OsbCoverage PROPERTIES
        TIMEOUT 240
        RUN_SERIAL TRUE
        PASS_REGULAR_EXPRESSION "VIEW3D_UI_CHECKS_OK"
        FAIL_REGULAR_EXPRESSION "VIEW3D_MEASURE_FAILED;VIEW3D_MODEL_IMPORT_FAILED;VIEW3D_CRS_FAILED;VIEW3D_FEATURE_TABLE_FAILED;VIEW3D_INSPECTOR_LOG_FAILED;VIEW3D_OSBG_SMOKE_FAILED"
        ENVIRONMENT "VIEW3D_TREE_PROBE=1;VIEW3D_OSBG_SWEEP=12;VIEW3D_OSBG_COVERAGE_STRICT=1"
        ENVIRONMENT_MODIFICATION "${_view3d_test_env_mods}")
endif()

if(MSVC)
    target_compile_options(View3D PRIVATE /W4 /permissive- /utf-8)
endif()
