#pragma once

#include "RenderTypes.h"

namespace View3D {

// 视口的显示端口。显示模式/网格/坐标轴/光照委托给共享场景；
// 背景与阴影属于单个视口的 compositor 配置。
class IViewportDisplay {
  public:
    virtual ~IViewportDisplay() = default;

    virtual bool setDisplayMode(ViewportDisplayMode mode) = 0;
    virtual ViewportDisplayMode displayMode() const noexcept = 0;
    virtual bool setBackgroundStyle(const ViewportBackgroundStyle& style) = 0;
    virtual ViewportBackgroundStyle backgroundStyle() const noexcept = 0;
    virtual bool setBackgroundMode(ViewportBackgroundMode mode) = 0;
    virtual ViewportBackgroundMode backgroundMode() const noexcept = 0;
    virtual bool setGridVisible(bool visible) = 0;
    virtual bool gridVisible() const noexcept = 0;
    virtual bool setAxesVisible(bool visible) = 0;
    virtual bool axesVisible() const noexcept = 0;
    virtual bool setEnvironmentStyle(const ViewportEnvironmentStyle& style) = 0;
    virtual ViewportEnvironmentStyle environmentStyle() const noexcept = 0;
    virtual bool setLightingStyle(const ViewportLightingStyle& style) = 0;
    virtual ViewportLightingStyle lightingStyle() const noexcept = 0;
    virtual bool setAmbientLightVisible(bool visible) = 0;
    virtual bool setMainLightVisible(bool visible) = 0;
    virtual bool setShadowStyle(const ViewportShadowStyle& style) = 0;
    virtual ViewportShadowStyle shadowStyle() const noexcept = 0;
    virtual bool setShadowsEnabled(bool enabled) = 0;
    virtual bool setShadowQuality(ViewportShadowQuality quality) = 0;
};

} // namespace View3D
