#pragma once

#include <QString>

namespace View3D {

// 3D Tiles 数据集的加载入口，由 3D Tiles 插件实现并发布。
// 宿主与 UI 命令只通过本接口触发加载，不依赖数据插件。
class ITilesetLoader {
  public:
    virtual ~ITilesetLoader() = default;

    // tilesetJson 为数据集清单文件（tileset.json）的绝对路径。
    virtual bool loadTileset(const QString& tilesetJson) = 0;
    // 撤下该数据集的全部瓦片并停止流式加载；再次 load 会重新建立会话。
    virtual bool unloadTileset(const QString& tilesetJson) = 0;
    virtual QString lastError() const = 0;
};

} // namespace View3D
