#pragma once

#include <QString>

namespace View3D {

// 倾斜摄影等外部数据集的加载入口，由数据插件实现并发布。
// 宿主与 UI 命令只通过本接口触发加载，不依赖任何数据插件。
class IOsbLoader {
  public:
    virtual ~IOsbLoader() = default;

    // directory 为数据集根目录（内含 metadata.xml 与 Data 瓦片树）。
    virtual bool loadDataset(const QString& directory) = 0;
    // 撤下该数据集的全部瓦片并停止流式加载；再次 load 会重新建立索引。
    virtual bool unloadDataset(const QString& directory) = 0;
    virtual QString lastError() const = 0;
};

} // namespace View3D
