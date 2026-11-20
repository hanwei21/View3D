#pragma once

#include "ModelReadTypes.h"

#include <QString>

class QWidget;

namespace View3DModel {

// 某个数据类别的 QFileDialog 过滤器，例如「模型文件 (*.glb *.gltf ...)」。
// 扩展名直接取自格式读取器注册表，避免过滤器与实际能读的格式漂移。
QString fileFilterForCategory(ImportFormatKind kind);
QString displayNameForCategory(ImportFormatKind kind);

// 导入选项对话框：单位/轴向/法线/节点。只有带网格的模型才问轴向、法线与节点，
// 点云与栅格只问单位。外壳没有坐标参考系服务，因此不提供源坐标系：留空即不重投影。
// 用户取消返回 false，*options 保持原值。
bool askImportOptions(QWidget* parent, ImportFormatKind kind, ModelImportOptions* options);

} // namespace View3DModel
