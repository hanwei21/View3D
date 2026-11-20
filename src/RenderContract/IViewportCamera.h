#pragma once

#include "CameraTypes.h"

#include <QString>

namespace View3D {

// 单个视口的相机与观察控制。多个视口共享同一个 IRenderScene。
class IViewportCamera {
  public:
    virtual ~IViewportCamera() = default;

    virtual bool cameraState(ViewportCameraState* state) const = 0;
    virtual bool setCameraState(const ViewportCameraState& state) = 0;
    // 二维（水平面）模式：锁定正俯视、上北下南，不接受旋转与视线方向位移。
    // 进入时记下当前相机，退出时恢复；投影强制为正交。
    virtual bool setPlanarMode(bool enabled) = 0;
    virtual bool planarMode() const noexcept = 0;
    // 球面模式相机：状态按 ECEF 语义读写（position/direction/up/target），
    // up 为椭球法线，与二维模式互斥。渲染端不支持时返回 false。
    virtual bool setGlobeModeEnabled(bool enabled)
    {
        static_cast<void>(enabled);
        return false;
    }
    virtual bool globeModeEnabled() const noexcept { return false; }
    virtual bool setStandardCameraView(StandardCameraView view) = 0;
    virtual bool setCameraProjectionMode(CameraProjectionMode mode) = 0;
    virtual CameraProjectionMode cameraProjectionMode() const = 0;
    virtual bool orbitCamera(double yawDegrees, double pitchDegrees) = 0;
    virtual bool panCamera(double horizontalPixels, double verticalPixels) = 0;
    virtual bool zoomCamera(double wheelSteps) = 0;
    virtual bool zoomCameraAt(double xPixels, double yPixels, double wheelSteps) = 0;
    virtual bool moveCameraLocal(double rightUnits, double upUnits, double forwardUnits) = 0;
    virtual bool moveCameraContinuously(double rightAxis, double upAxis, double forwardAxis,
                                        double elapsedSeconds, bool accelerated = false) = 0;
    virtual NavigationSettings navigationSettings() const noexcept = 0;
    virtual bool setNavigationSettings(const NavigationSettings& settings) = 0;
    virtual bool setClippingState(const ViewportClippingState& state) = 0;
    virtual ViewportClippingState clippingState() const noexcept = 0;
    virtual bool focusModelLayer(const QString& layerKey) = 0;
    virtual bool focusModelLayerNode(const QString& layerKey, std::uint32_t nodeIndex) = 0;
    virtual bool focusTileGroup(const QString& tileGroupKey) = 0;
    virtual bool focusAllVisible() = 0;
};

} // namespace View3D
