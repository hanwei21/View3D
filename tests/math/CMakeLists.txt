find_package(Qt6 6.8 REQUIRED COMPONENTS Core)

# 测量数学的纯函数回归：直接把 MeasurementMath.cpp 编进测试可执行文件，
# 不起 Qt 事件循环、不建渲染上下文，因此能在任何机器上稳定判定轴口径。
add_executable(View3DMeasurementMathTests
    MeasurementMathTests.cpp
    "${CMAKE_SOURCE_DIR}/src/Views/ToolsPlugin/MeasurementMath.cpp"
    "${CMAKE_SOURCE_DIR}/src/Views/ToolsPlugin/MeasurementMath.h"
    "${CMAKE_SOURCE_DIR}/src/Views/ToolsPlugin/MeasurementTypes.h")
target_include_directories(View3DMeasurementMathTests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/Views/ToolsPlugin"
    "${CMAKE_SOURCE_DIR}/src/RenderContract")
target_link_libraries(View3DMeasurementMathTests PRIVATE Qt6::Core)
target_compile_definitions(View3DMeasurementMathTests PRIVATE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
set_target_properties(View3DMeasurementMathTests PROPERTIES
    FOLDER "View3D/Tests")
if(MSVC)
    target_compile_options(View3DMeasurementMathTests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME View3D.MeasureMath COMMAND View3DMeasurementMathTests)
# 测试可执行文件在 bin 目录下运行，需要能找到 Qt6Core.dll。
get_target_property(_math_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _math_qt_core_location)
    get_target_property(_math_qt_core_location Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(_math_qt_core_dir "${_math_qt_core_location}" DIRECTORY)
set_tests_properties(View3D.MeasureMath PROPERTIES
    TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_math_qt_core_dir}")
