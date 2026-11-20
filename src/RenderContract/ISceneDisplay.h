#pragma once

#include "RenderTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace View3D {

// 共享场景的显示端口：显示模式、环境与光照属于场景，所有视口一致。
class ISceneDisplay {
  public:
    virtual ~ISceneDisplay() = default;

    virtual bool setDisplayMode(ViewportDisplayMode mode) = 0;
    virtual ViewportDisplayMode displayMode() const noexcept = 0;
    virtual bool setGridVisible(bool visible) = 0;
    virtual bool gridVisible() const noexcept = 0;
    virtual bool setAxesVisible(bool visible) = 0;
    virtual bool axesVisible() const noexcept = 0;
    // 二维（水平面）模式下坐标轴只画水平两轴，隐藏指向相机的高程轴。
    virtual bool setAxesPlanar(bool planar) = 0;
    virtual bool axesPlanar() const noexcept = 0;
    virtual bool setEnvironmentStyle(const ViewportEnvironmentStyle& style) = 0;
    virtual ViewportEnvironmentStyle environmentStyle() const noexcept = 0;
    virtual bool setLightingStyle(const ViewportLightingStyle& style) = 0;
    virtual ViewportLightingStyle lightingStyle() const noexcept = 0;
    virtual bool setAmbientLightVisible(bool visible) = 0;
    virtual bool setMainLightVisible(bool visible) = 0;

    // ── 球面（WGS84 椭球）模式：场景级范式开关，所有视口一致 ──────────────
    // 打开后带 contentOrigin 的瓦片按 ECEF 摆放，渲染原点跟随活动视口相机，
    // 显示椭球底面；关闭回到局部切平面行为。与二维模式互斥，由视图编排负责。
    // 渲染端不支持时返回 false，调用方据此降级。
    virtual bool setGlobeModeEnabled(bool enabled)
    {
        static_cast<void>(enabled);
        return false;
    }
    virtual bool globeModeEnabled() const noexcept { return false; }
    // 当前渲染原点（ECEF 米）：球面模式下渲染空间坐标 + 该值 = ECEF。
    // 未打开球面模式时返回 false。
    virtual bool globeRenderOrigin(std::array<double, 3>* ecefMeters) const
    {
        static_cast<void>(ecefMeters);
        return false;
    }
    // 模式切换完成后的通知（触发线程上同步执行）；token 用于移除，
    // 返回 0 表示不支持监听。数据插件据此重建流式会话。
    virtual std::uint64_t addGlobeModeChangeListener(
        std::function<void(bool enabled)> callback)
    {
        static_cast<void>(callback);
        return 0;
    }
    virtual void removeGlobeModeChangeListener(std::uint64_t token)
    {
        static_cast<void>(token);
    }
    // 球面默认底图替换（在线影像用）：data 为 width×height 的 RGBA8 等距圆柱
    // 纹理（宽高比 2:1，首行是北极）。渲染端立即换纹理，之后创建的球面也用
    // 这份数据；球面表面尚未创建时只保存。尺寸不合法或不支持返回 false，
    // 调用方保留内置静态底图。
    virtual bool setGlobeBasemapImage(int width, int height,
                                      const std::vector<unsigned char>& rgba8)
    {
        static_cast<void>(width);
        static_cast<void>(height);
        static_cast<void>(rgba8);
        return false;
    }
};

} // namespace View3D
