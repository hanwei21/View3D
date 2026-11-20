#pragma once

#include "ModelReadTypes.h"

#include <QString>

namespace View3DModel {

class AssimpModelImporter final {
  public:
    ModelReadResult importFile(const QString& path,
                                 const ModelImportOptions& options) const;
    // 本次构建的 assimp 实际编译进来的导入格式。清单在运行时向 assimp 自己查询，
    // 因此不会因为版本变化而与代码里的白名单脱节。
    static const QStringList& supportedSuffixes();
    static bool supportsExtension(const QString& suffix);
};

} // namespace View3DModel
