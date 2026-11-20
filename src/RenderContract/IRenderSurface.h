#pragma once

#include "IRenderScene.h"
#include "IViewportCamera.h"
#include "IViewportCapture.h"
#include "IViewportDisplay.h"
#include "IViewportOverlay.h"
#include "IViewportPicking.h"

#include <QSize>

class QWidget;

namespace View3D {

// 单个视口表面：窗口 + 相机 + 观察控制。内容与 GPU 资源在 IRenderScene 中。
class IRenderSurface : public IViewportCamera,
                       public IViewportOverlay,
                       public IViewportCapture,
                       public IViewportPicking,
                       public IViewportDisplay {
  public:
    ~IRenderSurface() override = default;

    virtual QWidget* widget() = 0;
    virtual void setRenderWindowVisible(bool visible) = 0;
    virtual bool ready() const noexcept = 0;
    virtual IRenderScene* scene() const = 0;
    // 窗口首次显示/停靠布局恢复后，按控件尺寸与 DPR 重新同步原生渲染分辨率。
    virtual void refreshRenderResolution() = 0;
    virtual QSize renderPixelSize() const = 0;
};

} // namespace View3D
