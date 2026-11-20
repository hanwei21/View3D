#pragma once

#include <IRenderSurface.h>
#include <QElapsedTimer>
#include <QPoint>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEngine;
class QResizeEvent;
class QWheelEvent;

namespace Ogre {
class Camera;
class CompositorWorkspace;
class ManualObject;
class Root;
class SceneManager;
class Window;
class HlmsUnlitDatablock;
}

namespace View3D {

class OgreRuntime;
class OgreScene;

struct ViewportShadowRuntimeState {
    bool enabled = false;
    ViewportShadowQuality requestedQuality = ViewportShadowQuality::Balanced;
    ViewportShadowQuality actualQuality = ViewportShadowQuality::Balanced;
    ViewportShadowFilter filter = ViewportShadowFilter::Pcf3x3;
    std::uint8_t splitCount = 0;
    std::array<std::uint32_t, 4> resolution {};
    bool workspaceUsesShadowNode = false;
    bool mainLightCastsShadows = false;
    std::uint32_t activeWorkspaceCount = 0;
    std::uint32_t workspaceSwitchCount = 0;
    std::uint32_t fallbackCount = 0;
};

// 单个视口：窗口、相机、compositor workspace、量测/剖切叠加层。
// 场景内容完全由共享的 OgreScene 持有。
class OgreViewportSurface final : public QWidget, public IRenderSurface {
  public:
    OgreViewportSurface(OgreRuntime& runtime, OgreScene& scene,
                        QWidget* parent = nullptr);
    ~OgreViewportSurface() override;

    bool initialize(const QString& runtimeDirectory);
    void shutdown();
    void prepareFrame();
    // 渲染线程只读这个原子镜像；Qt 可见性状态必须在 UI 线程采样。
    bool renderActive() const noexcept { return m_renderActive.load(std::memory_order_relaxed); }

    QWidget* widget() override { return this; }
    void setRenderWindowVisible(bool visible) override;
    bool ready() const noexcept override;
    IRenderScene* scene() const override;
    void refreshRenderResolution() override { resizeRenderWindow(); }
    QSize renderPixelSize() const override;

    bool cameraState(ViewportCameraState* state) const override;
    bool setCameraState(const ViewportCameraState& state) override;
    bool setPlanarMode(bool enabled) override;
    bool planarMode() const noexcept override { return m_planarMode; }
    bool setGlobeModeEnabled(bool enabled) override;
    bool globeModeEnabled() const noexcept override;
    // 渲染线程：所有视口 prepareFrame 之后调用，把相机位置统一回「ECEF − 最终锚点」。
    void syncGlobeCamera();
    bool setStandardCameraView(StandardCameraView view) override;
    bool setCameraProjectionMode(CameraProjectionMode mode) override;
    CameraProjectionMode cameraProjectionMode() const override;
    bool orbitCamera(double yawDegrees, double pitchDegrees) override;
    bool panCamera(double horizontalPixels, double verticalPixels) override;
    bool zoomCamera(double wheelSteps) override;
    bool zoomCameraAt(double xPixels, double yPixels, double wheelSteps) override;
    bool moveCameraLocal(double rightUnits, double upUnits, double forwardUnits) override;
    bool moveCameraContinuously(double rightAxis, double upAxis, double forwardAxis,
                                double elapsedSeconds, bool accelerated = false) override;
    NavigationSettings navigationSettings() const noexcept override
    {
        return m_navigationSettings;
    }
    bool setNavigationSettings(const NavigationSettings& settings) override;
    bool setClippingState(const ViewportClippingState& state) override;
    ViewportClippingState clippingState() const noexcept override
    {
        return m_clippingState;
    }
    bool focusModelLayer(const QString& layerKey) override;
    bool focusModelLayerNode(const QString& layerKey, std::uint32_t nodeIndex) override;
    bool focusTileGroup(const QString& tileGroupKey) override;
    bool focusAllVisible() override;

    bool setOverlay(const std::vector<std::array<double, 3>>& points,
                    bool connectPoints) override;
    void clearOverlay() override;
    bool hasOverlay() const noexcept override;

    QImage captureImage(const QSize& outputSize = QSize(),
                        const CaptureOptions& options = CaptureOptions()) override;
    QImage captureImageOffscreen(const QSize& outputSize,
                                 const CaptureOptions& options) override;
    QImage sceneSnapshot() override { return captureImage(); }

    PickListenerId addPickListener(
        std::function<void(const ViewportPickResult&)> listener) override;
    bool removePickListener(PickListenerId id) override;
    PickListenerId addHoverPickListener(
        std::function<void(const ViewportPickResult&)> listener) override;
    bool removeHoverPickListener(PickListenerId id) override;
    void setRectSelectCallback(
        std::function<void(const QList<QString>&, bool additive)> callback) override;

    bool setDisplayMode(ViewportDisplayMode mode) override;
    ViewportDisplayMode displayMode() const noexcept override;
    bool setBackgroundStyle(const ViewportBackgroundStyle& style) override;
    ViewportBackgroundStyle backgroundStyle() const noexcept override
    {
        return m_backgroundStyle;
    }
    bool setBackgroundMode(ViewportBackgroundMode mode) override;
    ViewportBackgroundMode backgroundMode() const noexcept override
    {
        return m_backgroundStyle.mode;
    }
    bool setGridVisible(bool visible) override;
    bool gridVisible() const noexcept override;
    bool setAxesVisible(bool visible) override;
    bool axesVisible() const noexcept override;
    bool setEnvironmentStyle(const ViewportEnvironmentStyle& style) override;
    ViewportEnvironmentStyle environmentStyle() const noexcept override;
    bool setLightingStyle(const ViewportLightingStyle& style) override;
    ViewportLightingStyle lightingStyle() const noexcept override;
    bool setAmbientLightVisible(bool visible) override;
    bool setMainLightVisible(bool visible) override;
    bool setShadowStyle(const ViewportShadowStyle& style) override;
    ViewportShadowStyle shadowStyle() const noexcept override { return m_shadowStyle; }
    bool setShadowsEnabled(bool enabled) override;
    bool setShadowQuality(ViewportShadowQuality quality) override;

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    QPaintEngine* paintEngine() const override;
    bool event(QEvent* event) override;

  private:
    struct ShadowPreset {
        ViewportShadowQuality quality;
        ViewportShadowFilter filter;
        std::uint8_t splitCount;
        std::array<std::uint32_t, 4> resolution;
    };

    bool createBackgroundResources();
    bool updateBackgroundConstants();
    bool createViewportWorkspace();
    bool createShadowNodeDefinitions();
    void destroyShadowNodeDefinitions();
    bool createViewportWorkspaceDefinitions();
    void destroyViewportWorkspaceDefinitions();
    bool switchViewportWorkspace(bool enabled, ViewportShadowQuality quality);
    std::size_t shadowQualityIndex() const noexcept;
    static ShadowPreset shadowPreset(ViewportShadowQuality quality) noexcept;
    void destroyBackgroundResources();

    // 该视口在共享场景里占用的可见性位；位用尽时为“全部可见”。
    std::uint32_t viewVisibilityMask() const noexcept;

    bool setMeasurementOverlay(const std::vector<std::array<double, 3>>& points,
                               bool connectPoints);
    void clearMeasurementOverlay();
    void clearMeasurementOverlayResources();
    bool rebuildMeasurementOverlay();
    void clearClippingHelperResources();
    bool rebuildClippingHelper();
    bool applyClippingState(const ViewportClippingState& state);

    ViewportPickResult pickAtPosition(const QPointF& position) const;
    // 视线与地面(y=0)的交点：悬停没碰到模型时给坐标读数一个可显示的值。
    bool fillGroundPosition(const QPointF& position, ViewportPickResult* result) const;
    bool viewportPointOnTargetPlane(double xPixels, double yPixels,
                                    std::array<double, 3>* point) const;
    bool pickScenePointAt(double xPixels, double yPixels,
                          std::array<double, 3>* point) const;
    // fallbackToView：拾取不到（点在天空）时，用当前视线前方、原枢轴距离处的点。
    bool setNavigationPivotAt(double xPixels, double yPixels, bool fallbackToView);
    void updateClipDistances(double distanceToTarget);
    // 广播一次拾取：先拷贝监听者快照，允许回调里注销自己。
    void dispatchPick(const ViewportPickResult& result);
    void dispatchHover(const ViewportPickResult& result);
    bool setNavigationTargetFromViewportPoint(double xPixels, double yPixels);
    bool focusBounds(const RenderBounds& bounds);
    // 二维模式：把相机压回正俯视姿态，或把请求的相机状态裁剪成平面姿态。
    void applyPlanarPose();
    bool applyPlanarCameraState(const ViewportCameraState& state);
    // 退出二维：保住当前位置与缩放，只把俯仰/方位/投影换回进入前的风格。
    bool restoreFromPlanar(const ViewportCameraState& previous);
    // 二维模式的聚焦：按水平投影范围铺满视野，而不是斜视整个立体包围盒。
    bool focusBoundsPlanar(const RenderBounds& bounds);

    // 球面相机：真值始终是 ECEF 双精度（m_globePositionEcef/m_globeTargetEcef），
    // Ogre 相机只保存相对锚点的 float 姿态。up 取椭球法线，地平线不滚转。
    bool applyGlobeCameraState(const ViewportCameraState& state);
    bool applyGlobeCamera();
    bool globeOrbit(double yawDegrees, double pitchDegrees);
    bool globePan(double horizontalPixels, double verticalPixels);
    bool globeZoom(double wheelSteps);
    bool globeMoveLocal(double rightUnits, double upUnits, double forwardUnits);
    bool globeFocusBounds(const RenderBounds& bounds);
    bool globeSurfacePointAt(double xPixels, double yPixels,
                             std::array<double, 3>* ecefPoint) const;
    void updateGlobeClipDistances();
    void clampGlobeCameraAltitude();

    void resizeRenderWindow();
    // UI 线程：把控件可见/最小化状态镜像到 m_renderActive。
    void updateRenderActive();

    OgreRuntime& m_runtime;
    OgreScene& m_scene;
    Ogre::Root* m_root = nullptr;
    Ogre::SceneManager* m_sceneManager = nullptr;
    std::string m_instanceSuffix;
    std::uint32_t m_visibilityBit = 0u;
    std::atomic_bool m_renderWindowVisible {true};
    std::atomic_bool m_readyFlag {false};
    std::atomic_bool m_renderActive {false};
    QTimer m_activityTimer;
    Ogre::Window* m_window = nullptr;
    Ogre::Camera* m_camera = nullptr;
    std::array<double, 3> m_cameraTarget {};
    bool m_planarMode = false;
    // 进入二维前的相机快照，退出时原样恢复（含投影模式）。
    ViewportCameraState m_planarReturnState {};
    bool m_planarReturnValid = false;
    bool m_globeMode = false;
    bool m_globeCameraValid = false;
    std::array<double, 3> m_globePositionEcef {};
    std::array<double, 3> m_globeTargetEcef {};
    double m_sceneRadius = 1000.0;
    Ogre::CompositorWorkspace* m_workspace = nullptr;

    std::string m_backgroundVertexProgramName;
    std::string m_backgroundFragmentProgramName;
    std::string m_backgroundMaterialName;
    std::string m_backgroundNodeName;
    std::string m_backgroundWorkspaceName;
    std::array<std::string, 3> m_shadowNodeNames;
    std::array<std::string, 3> m_shadowViewportNodeNames;
    std::array<std::string, 3> m_shadowWorkspaceNames;

    ViewportBackgroundStyle m_backgroundStyle;
    ViewportShadowStyle m_shadowStyle;
    ViewportShadowRuntimeState m_shadowRuntime;

    struct MeasurementOverlayCameraState {
        std::array<double, 3> position {};
        std::array<double, 3> direction {};
        std::array<double, 3> up {};
        // 球面锚点：测量点存 ECEF，叠加层按锚点换到渲染空间；锚点变化也要重建。
        std::array<double, 3> globeAnchor {};
        bool globe = false;
        double verticalFov = 0.0;
        double orthoHeight = 0.0;
        int projectionType = 0;
        int renderWidth = 0;
        int renderHeight = 0;

        bool operator==(const MeasurementOverlayCameraState& other) const
        {
            return position == other.position && direction == other.direction &&
                up == other.up && globeAnchor == other.globeAnchor &&
                globe == other.globe && verticalFov == other.verticalFov &&
                orthoHeight == other.orthoHeight &&
                projectionType == other.projectionType &&
                renderWidth == other.renderWidth &&
                renderHeight == other.renderHeight;
        }
    };
    void rebuildMeasurementOverlayGeometry();
    void updateMeasurementOverlayForCamera();
    MeasurementOverlayCameraState measurementOverlayCameraState() const;

    std::uint32_t m_requestedRenderWidth = 0u;
    std::uint32_t m_requestedRenderHeight = 0u;
    Ogre::ManualObject* m_measurementOverlayObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_measurementOverlayDatablock = nullptr;
    std::string m_measurementOverlayDatablockName;
    std::vector<std::array<double, 3>> m_measurementOverlayPoints;
    bool m_measurementOverlayConnectPoints = false;
    MeasurementOverlayCameraState m_measurementOverlayCamera;

    Ogre::ManualObject* m_clippingHelperObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_clippingHelperDatablock = nullptr;
    std::string m_clippingHelperDatablockName;
    ViewportClippingState m_clippingState;

    NavigationSettings m_navigationSettings;
    QPoint m_navigationLastPosition;
    Qt::MouseButton m_navigationButton = Qt::NoButton;
    QElapsedTimer m_navigationElapsed;
    QSet<int> m_navigationKeys;

    struct PickListener {
        PickListenerId id = 0;
        std::function<void(const ViewportPickResult&)> callback;
    };
    std::vector<PickListener> m_pickListeners;
    std::vector<PickListener> m_hoverListeners;
    PickListenerId m_nextPickListenerId = 1;
    qint64 m_lastHoverPickMs = 0;
    std::function<void(const QList<QString>&, bool)> m_rectSelectCallback;
    QPoint m_rectSelectStart;
    QPoint m_rectSelectCurrent;
    bool m_rectSelectActive = false;
    bool m_resizeLogged = false;
};

} // namespace View3D
