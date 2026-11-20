#pragma once

#include "ModelReadTypes.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstddef>

namespace View3DModel {

struct GltfExtensionDeclarations {
    bool recognized = false;
    QStringList used;
    QStringList required;
};

// 单文件 glTF/GLB 导入。解码用 Cesium Native 的 CesiumGltfReader，与 3D Tiles 插件
// 共用同一套访问器实现；外部 buffer/贴图按本地文件解析，不走网络。
class GltfModelImporter final {
  public:
    static GltfExtensionDeclarations inspectExtensionDeclarations(const std::byte* data,
                                                                  std::size_t size);
    static bool supportsExtension(const QString& extension);

    ModelReadResult importFile(const QString& path,
                               const ModelImportOptions& options = {}) const;
};

} // namespace View3DModel
