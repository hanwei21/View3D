include_guard(GLOBAL)

# assimp 6.0.4（vcpkg x64-windows-static-md）：FBX/DAE/OFF/SKP 等几十个建模格式的读取。
# 预编译树放在 third_party/assimp（include/lib），缺失时模型导入插件不编译 assimp 读取器。
# 与 OSG/Cesium 一样不消费 vcpkg 导出的 CMake 包：那些 targets 引用 debug/lib，
# 而本机只有 Release 静态库，走 find_package 会在 configure 阶段中止。
set(VIEW3D_ASSIMP_ROOT "${CMAKE_SOURCE_DIR}/third_party/assimp" CACHE PATH
    "assimp 预编译树根目录")
set(VIEW3D_ASSIMP_FOUND FALSE)

set(_assimp_main_lib "${VIEW3D_ASSIMP_ROOT}/lib/assimp-vc143-mt.lib")
if(NOT EXISTS "${VIEW3D_ASSIMP_ROOT}/include/assimp/Importer.hpp" OR NOT EXISTS "${_assimp_main_lib}")
    message(STATUS "View3D: 未找到 assimp（${VIEW3D_ASSIMP_ROOT}），模型导入插件不含 assimp 读取器")
    return()
endif()

# assimp 自带的第三方静态库（压缩、多边形化、XML）；逐个列名会漏，整目录取。
file(GLOB _assimp_libraries LIST_DIRECTORIES false "${VIEW3D_ASSIMP_ROOT}/lib/*.lib")
list(SORT _assimp_libraries)

add_library(View3D::Assimp STATIC IMPORTED GLOBAL)
set_target_properties(View3D::Assimp PROPERTIES
    IMPORTED_LOCATION "${_assimp_main_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${VIEW3D_ASSIMP_ROOT}/include")
# assimp 的头默认按静态链接展开（只有定义 ASSIMP_DLL 才出 dllimport），所以这里
# 不需要额外的导出宏；依赖闭包（zlib/minizip/poly2tri/polyclipping/pugixml）整目录带上。
set_property(TARGET View3D::Assimp APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    "${_assimp_libraries}" winmm.lib)

set(VIEW3D_ASSIMP_FOUND TRUE)
message(STATUS "View3D: 找到 assimp（${VIEW3D_ASSIMP_ROOT}）")
