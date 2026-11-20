#pragma once

#include "ModelReadTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace View3DModel {

// SHP/KML 矢量：点要素输出点图元（带要素标记四边形），线与多边形边界输出
// 带宽度的三角形带。通过 GDAL/OGR 的 C API 读取，并把每个 OGR 图层建成
// ModelMeshDto 的要素表（字段名 + 逐要素取值），每个要素生成独立 featureId
// 的图元，使点选拾取能直接返回真实属性（FEATURE 表 → ViewportPickResult.featureProperties）。
class GdalVectorImporter final {
  public:
    static QStringList suffixes();
    static bool supportsSuffix(const QString& suffix);
    static bool isVectorMagic(const QByteArray& prefix);

    ModelReadResult importFile(const QString& path,
                                 const ModelImportOptions& options) const;
};

} // namespace View3DModel
