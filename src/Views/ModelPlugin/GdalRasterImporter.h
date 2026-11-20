#pragma once

#include "ModelReadTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace View3DModel {

// GeoTIFF 栅格：多波段按 DOM 生成贴图网格，单波段按 DEM/DSM 生成地形网格。
// 读取通过 GDAL 的 C API 完成，避免把 GDAL 的 C++ ABI 带进插件边界。
class GdalRasterImporter final {
  public:
    static QStringList suffixes();
    static bool supportsSuffix(const QString& suffix);
    // GeoTIFF 的魔数覆盖普通 TIFF 与 BigTIFF，扩展名写错时也能认领。
    static bool isRasterMagic(const QByteArray& prefix);

    ModelReadResult importFile(const QString& path,
                                 const ModelImportOptions& options) const;
};

} // namespace View3DModel
