#pragma once

#include "ModelReadTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace View3DModel {

// E57 点云。读取由 E57Format（BSL-1.0）的 Simple API 完成；它负责 XML/压缩向量
// 解析，我们只处理坐标、位姿、颜色和点预算。
class E57PointCloudImporter final {
  public:
    static QStringList suffixes();
    static bool supportsSuffix(const QString& suffix);
    // E57 文件头固定以 "ASTM-E57" 开头，扩展名写错时仍能正确认领。
    static bool isCloudMagic(const QByteArray& prefix);

    ModelReadResult importFile(const QString& path,
                                 const ModelImportOptions& options) const;
};

} // namespace View3DModel
