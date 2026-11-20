#pragma once

#include "ModelReadTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace View3DModel {

// LAS / LAZ 点云。解析与解压缩都由 LASzip 的 C API 完成（Apache-2.0）：它自带完整
// 的 LAS 头解析和 LAZ 算术解码，所以不需要引入 GPL-2.0 的 LASlib。
class LasPointCloudImporter final {
  public:
    static QStringList suffixes();
    static bool supportsSuffix(const QString& suffix);
    // LAS 文件以 "LASF" 开头，靠魔数认领可以覆盖扩展名写错的情况。
    static bool isCloudMagic(const QByteArray& prefix);

    ModelReadResult importFile(const QString& path,
                                 const ModelImportOptions& options) const;
};

} // namespace View3DModel
