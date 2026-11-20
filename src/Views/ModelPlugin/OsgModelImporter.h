#pragma once

#include "ModelReadTypes.h"

#include <QString>
#include <QStringList>

namespace View3DModel {

// 整文件 OpenSceneGraph 模型读取：OSG 特有的 .osg/.osgt/.ive/OpenFlight 等格式，
// assimp 覆盖不到的那些走这条路。
class OsgModelImporter final {
  public:
    static QStringList suffixes();
    static bool supportsSuffix(const QString& suffix);
    ModelReadResult importFile(const QString& path, const ModelImportOptions& options) const;
};

} // namespace View3DModel
