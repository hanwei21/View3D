#pragma once

#include "IRenderScene.h"
#include "IRenderSurface.h"
#include "RenderTypes.h"

#include <QList>

namespace View3D {

enum class ViewportLayout { Single, Dual, Quad };

// 多视口布局端口：由三维内容视图实现并注册为服务，业务侧不再 downcast QWidget。
class IViewportLayout {
  public:
    virtual ~IViewportLayout() = default;

    virtual bool initialize() = 0;
    virtual IRenderScene* scene() const = 0;
    virtual IRenderSurface* primaryView() const = 0;
    virtual IRenderSurface* activeView() const = 0;
    virtual int layoutViewportCount() const = 0;
    virtual QList<IRenderSurface*> views() const = 0;
    virtual bool setLayout(ViewportLayout layout) = 0;
    virtual bool setCameraSync(bool enabled) = 0;
    virtual bool cameraSyncEnabled() const = 0;
    // 二维/三维模式属于布局而不是单个视口：新建视口要继承当前模式。
    virtual bool setPlanarMode(bool enabled) = 0;
    virtual bool planarMode() const = 0;
    // 球面模式同属布局级范式：所有视口一起切换，新建视口继承模式与相机基准；
    // 相机局部↔ECEF 换算与非瓦片图层的隐藏/恢复由布局编排（与二维互斥）。
    // 渲染端不支持时返回 false，调用方据此降级。
    virtual bool setGlobeModeEnabled(bool enabled)
    {
        static_cast<void>(enabled);
        return false;
    }
    virtual bool globeModeEnabled() const { return false; }
    virtual void applySettings(const ViewportSettings& settings) = 0;
};

} // namespace View3D
