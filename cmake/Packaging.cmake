include_guard(GLOBAL)

# Windows 安装包（CPack + NSIS）。
#
# 部署思路：各渲染/数据运行库已由 POST_BUILD 统一收敛到 ${CMAKE_BINARY_DIR}/bin，
# 且所有数据目录（proj-data/gdal-data/Hlms/插件）都按 applicationDirPath() 相对定位，
# 因此把整棵 bin 树原样装到安装根目录即可保证相对布局不变，无需逐目标 install。
#
# 只剔除三类文件：调试/链接产物、测试专用产物、运行期用户状态。
# 用户状态（参照系列、UI 布局）首次保存时才生成，不能把开发机的状态打进包里。

if(NOT WIN32)
    return()
endif()

set(VIEW3D_PACKAGE_EXCLUDES
    "*.pdb"
    "*.ilk"
    "*.exp"
    "*.log"
    "stage2_probe.exe"
    "View3DMeasurementMathTests.exe"
    "ui_config.test.xml"
    "crs_library_test.json"
    "crs_library.json"
    "layout_state.ini")

set(_view3d_exclude_args "")
foreach(_pat IN LISTS VIEW3D_PACKAGE_EXCLUDES)
    list(APPEND _view3d_exclude_args PATTERN "${_pat}" EXCLUDE)
endforeach()

install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
    DESTINATION "."
    ${_view3d_exclude_args})

# ---- VC++ 运行库 -----------------------------------------------------------
# 工程以 /MD 链接，bin 里没有 app-local 的 vcruntime/msvcp；干净机器必须先装
# VC++ redist。随包附带并在安装末尾静默执行（/norestart，版本已存在时自动跳过）。
set(VIEW3D_VC_REDIST_EXE "" CACHE FILEPATH
    "随安装包附带的 vc_redist.x64.exe（留空则自动在 VS 的 Redist 目录里找）")
if(NOT VIEW3D_VC_REDIST_EXE)
    set(_view3d_redist_roots "")
    if(DEFINED ENV{VCToolsRedistDir})
        get_filename_component(_r "$ENV{VCToolsRedistDir}" DIRECTORY)
        list(APPEND _view3d_redist_roots "${_r}")
    endif()
    # 从 cl.exe 路径回溯到 VC 根，兜底覆盖未走 VsDevCmd 的配置。
    set(_d "${CMAKE_CXX_COMPILER}")
    foreach(_i RANGE 12)
        get_filename_component(_d "${_d}" DIRECTORY)
        get_filename_component(_leaf "${_d}" NAME)
        if(_leaf STREQUAL "VC")
            list(APPEND _view3d_redist_roots "${_d}/Redist/MSVC")
            break()
        endif()
    endforeach()
    set(_view3d_redist_best "")
    foreach(_root IN LISTS _view3d_redist_roots)
        file(GLOB _cands "${_root}/*/vc_redist.x64.exe")
        list(SORT _cands)
        foreach(_c IN LISTS _cands)
            get_filename_component(_vdir "${_c}" DIRECTORY)
            get_filename_component(_vname "${_vdir}" NAME)
            if(_vname MATCHES "^14\\.")
                set(_view3d_redist_best "${_c}")
            endif()
        endforeach()
    endforeach()
    if(_view3d_redist_best)
        set(VIEW3D_VC_REDIST_EXE "${_view3d_redist_best}" CACHE FILEPATH
            "随安装包附带的 vc_redist.x64.exe" FORCE)
    endif()
endif()

if(VIEW3D_VC_REDIST_EXE AND EXISTS "${VIEW3D_VC_REDIST_EXE}")
    install(FILES "${VIEW3D_VC_REDIST_EXE}" DESTINATION "redist")
    # 安装段末尾执行：装完运行库即删掉这个 18MB 的引导器，保持安装目录干净。
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "ExecWait '\"$INSTDIR\\redist\\vc_redist.x64.exe\" /install /quiet /norestart'\nDelete \"$INSTDIR\\redist\\vc_redist.x64.exe\"\nRMDir \"$INSTDIR\\redist\"\n")
    message(STATUS "View3D: 安装包附带 VC++ 运行库 ${VIEW3D_VC_REDIST_EXE}")
else()
    message(WARNING
        "View3D: 未找到 vc_redist.x64.exe，安装包将不含 VC++ 运行库；"
        "干净机器上需先手动安装，或用 -DVIEW3D_VC_REDIST_EXE=<路径> 指定。")
endif()

# ---- CPack / NSIS 基本参数 -------------------------------------------------
set(CPACK_PACKAGE_NAME "View3D")
set(CPACK_PACKAGE_VENDOR "View3D")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "View3D")
set(CPACK_PACKAGE_FILE_NAME "View3D-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/out/installer")
set(CPACK_GENERATOR "NSIS")

# 显示名含中文。CPack 的 DEFINES 注入点在 Name 之后，无法提前写 `Unicode true`
# （NSIS 禁止在写入字符串后再改字符集），故不改动字符集：makensis 按本机 ANSI
# 代码页（中文 Windows 为 GBK）编译，目标机器即中文 Windows，显示正常。
set(CPACK_NSIS_PACKAGE_NAME "View3D")
set(CPACK_NSIS_DISPLAY_NAME "View3D 三维预览")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_INSTALLED_ICON_NAME "View3D.exe")
set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "View3D.exe")

# 开始菜单 + 桌面快捷方式（由 CPACK_PACKAGE_EXECUTABLES / CPACK_CREATE_DESKTOP_LINKS 生成）。
# 整棵 bin 树平铺装到安装根目录，exe 在 $INSTDIR\View3D.exe 而非默认的 bin\ 子目录；
# 不改这个变量，快捷方式与完成页"运行"会指向不存在的 $INSTDIR\bin\View3D.exe。
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_PACKAGE_EXECUTABLES "View3D" "View3D")
set(CPACK_CREATE_DESKTOP_LINKS "View3D")

# EXTRA_INSTALL_COMMANDS 里含 NSIS 反斜杠路径（$INSTDIR\redist\...）。默认编码不做
# CMake 转义，cpack 重读 CPackConfig.cmake 时 \r/\v 会被当转义序列报错；逐字编码修复。
set(CPACK_VERBATIM_VARIABLES ON)

include(CPack)