#pragma once

#include "CoreTypes.h"

#include <QList>
#include <QString>

namespace View3D {

// 数据插件注册的“打开数据”入口。目录树只提供统一菜单与文件对话框，
// 具体文件类型、解析与图层登记都留在数据插件里；后续新增数据类型只需
// 实现本接口并登记，不必改动场景树。
class IDataOpener {
  public:
    virtual ~IDataOpener() = default;

    // 文件类型名称，例如“倾斜摄影数据集”。
    virtual QString displayName() const = 0;
    // QFileDialog 过滤器，例如 "倾斜摄影数据集 (metadata.xml Metadata.xml)"。
    virtual QString fileFilter() const = 0;
    // path 是用户选中的文件（打开器可以取其所在目录）；parentGroup 为空表示场景根。
    virtual bool openData(const QString& path, const LayerId& parentGroup,
                          QString* error) = 0;
};

class IDataOpenRegistry {
  public:
    virtual ~IDataOpenRegistry() = default;

    virtual void addDataOpener(IDataOpener* opener) = 0;
    virtual void removeDataOpener(IDataOpener* opener) = 0;
    virtual QList<IDataOpener*> dataOpeners() const = 0;
};

} // namespace View3D
