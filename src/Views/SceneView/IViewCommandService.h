#pragma once

#include "ICommandService.h"

#include <CameraTypes.h>
#include <RenderTypes.h>

namespace RF::PluginEngine {

class IViewCommandService : public ICommandService {
  public:
    ~IViewCommandService() override = default;
    virtual void setStandardView(View3D::StandardCameraView) = 0;
    virtual void setDisplayMode(View3D::ViewportDisplayMode) = 0;
    virtual void setBackgroundMode(View3D::ViewportBackgroundMode) = 0;
    virtual void setGridVisible(bool) = 0;
    virtual void setAxesVisible(bool) = 0;
    virtual void setAmbientLightVisible(bool) = 0;
    virtual void setMainLightVisible(bool) = 0;
    virtual void setShadowsEnabled(bool) = 0;
    virtual void setShadowQuality(View3D::ViewportShadowQuality) = 0;
    virtual void setProjectionMode(View3D::CameraProjectionMode) = 0;
    // 二维（水平面）/三维切换：锁定正俯视、上北下南，不接受旋转。
    virtual void setPlanarMode(bool enabled) = 0;
    // 球面（WGS84 椭球）模式：只对 3D Tiles 数据生效，与二维模式互斥。
    virtual void setGlobeMode(bool enabled) = 0;
    virtual bool setViewportLayout(int layout) = 0;
    virtual bool setCameraSync(bool enabled) = 0;
    virtual void captureViewport() = 0;
    virtual void showNavigationSettings() = 0;
    virtual void setNavigationHelpVisible(bool) = 0;
    virtual void setCameraStatusVisible(bool) = 0;
    virtual void copyCameraState() = 0;
    virtual void locateCameraExactly() = 0;
    virtual void pasteCameraState() = 0;
    virtual bool isFullscreen() const = 0;
    virtual void setFullscreen(bool enabled) = 0;
    virtual void setQualityPreset(int preset) = 0;
};

} // namespace RF::PluginEngine
