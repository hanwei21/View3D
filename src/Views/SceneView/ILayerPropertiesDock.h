#pragma once

#include <CoreTypes.h>

namespace View3D {

// 图层属性面板对宿主和其他插件暴露的最小联动接口：只读当前绑定的图层，
// 或把面板切到某个图层。所有属性写入仍然只有 ISceneLayerService 一条路，
// 这里不提供写入口，避免外部绕过服务直接改场景。
class ILayerPropertiesDock {
  public:
    virtual ~ILayerPropertiesDock() = default;

    virtual LayerId currentLayer() const = 0;
    virtual void setCurrentLayer(const LayerId& layerId) = 0;
};

} // namespace View3D
