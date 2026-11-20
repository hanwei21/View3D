#pragma once

#include <IRenderScene.h>
#include <IRenderService.h>
#include <IRenderSurface.h>
#include <ISceneLayerService.h>
#include <IViewportLayout.h>
#include <RenderTypes.h>

#include <QHash>
#include <QPoint>
#include <QSet>
#include <QWidget>

#include <memory>

class QGridLayout;
class QLabel;
class QTimer;
class QEvent;

namespace RF::PluginEngine {
class IPluginContainer;
}

namespace View3D {
class IViewportMenuRegistry;
}

namespace View3DPlugins {

class SceneLayerService;

class ViewportContainer final : public QWidget, public View3D::IViewportLayout {
  public:
    explicit ViewportContainer(RF::PluginEngine::IPluginContainer* container,
                               QWidget* parent = nullptr);
    ~ViewportContainer() override;

    bool initialize() override;
    View3D::IRenderScene* scene() const override { return m_scene; }
    View3D::IRenderSurface* primaryView() const override { return m_primary; }
    View3D::IRenderSurface* activeView() const override { return m_activeView; }
    int layoutViewportCount() const override;
    QList<View3D::IRenderSurface*> views() const override { return m_views; }
    bool setLayout(View3D::ViewportLayout layout) override;
    bool setCameraSync(bool enabled) override;
    bool cameraSyncEnabled() const override { return m_cameraSync; }
    bool setPlanarMode(bool enabled) override;
    // 状态以主视口为准：视口才是模式的真正持有者，布局只做聚合与继承。
    bool planarMode() const override
    {
        return m_primary ? m_primary->planarMode() : m_settings.planarMode;
    }
    // 球面模式：场景是范式持有者；布局负责相机局部↔ECEF 换算与图层编排。
    bool setGlobeModeEnabled(bool enabled) override;
    bool globeModeEnabled() const override
    {
        return m_scene ? m_scene->globeModeEnabled() : m_settings.globeMode;
    }
    void applySettings(const View3D::ViewportSettings& settings) override;
    bool setActiveView(View3D::IRenderSurface* view);

  private:
    View3D::IRenderSurface* createViewport();
    void ensureViewportCount(int count);
    // 每个视口底部一条坐标读数：把渲染窗口和读数条包进一个格子容器，
    // 多视口时各显示自己鼠标位置的坐标。
    QWidget* cellFor(View3D::IRenderSurface* view);
    void updateCoordinateReadout(View3D::IRenderSurface* view,
                                 const View3D::ViewportPickResult& result);
    // 局部切平面相机 ↔ ECEF 相机：点走 IGeoReference，方向用同一映射的
    // 线性部分；参照系未就绪时返回 false，调用方降级到默认视角。
    bool convertCameraToGlobe(const View3D::ViewportCameraState& local,
                              View3D::ViewportCameraState* globe) const;
    bool convertCameraToLocal(const View3D::ViewportCameraState& globe,
                              View3D::ViewportCameraState* local) const;
    static View3D::ViewportCameraState defaultGlobeCameraState(
        const View3D::ViewportCameraState& base);
    static View3D::ViewportCameraState defaultLocalCameraState();
    // 球面模式支持瓦片（3D Tiles/倾斜摄影）与模型导入（球面重烘焙）；影像与矢量
    // 仍是平面覆盖层，没有球面摆放路径，进球面时暂存可见性后隐藏。
    void ensureLayerService();
    void setFlatOnlyLayersHidden(bool hidden);
    bool hideFlatOnlyLayer(const View3D::LayerId& layerId);
    void restoreFlatOnlyLayers();
    void rebuildLayout();
    void applyVisibility();
    void syncCameras();
    void applySettingsToView(View3D::IRenderSurface* view);
    void updateCameraSyncTimer();
    void showViewportMenu(View3D::IRenderSurface* view, const QPoint& localPosition,
                          const QPoint& globalPosition);
    View3D::IRenderSurface* viewForWidget(QObject* watched) const;
    bool eventFilter(QObject* watched, QEvent* event) override;

    RF::PluginEngine::IPluginContainer* m_pluginContainer = nullptr;
    View3D::IViewportMenuRegistry* m_viewportMenuRegistry = nullptr;
    SceneLayerService* m_layerService = nullptr;
    // 图层服务回调在容器析构后仍可能被触发，用一个存活标志兜底。
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QSet<View3D::LayerId> m_globeHiddenLayers;
    // 进入球面前的平面相机：退出时 ECEF→局部换算失败（无地理参考）的兜底。
    QHash<View3D::IRenderSurface*, View3D::ViewportCameraState> m_globeFlatCameras;
    QPoint m_rightClickStart;
    View3D::IRenderSurface* m_rightClickView = nullptr;
    bool m_rightClickArmed = false;
    View3D::IRenderService* m_renderService = nullptr;
    View3D::IRenderScene* m_scene = nullptr;
    QGridLayout* m_grid = nullptr;
    QTimer* m_cameraSyncTimer = nullptr;
    View3D::IRenderSurface* m_primary = nullptr;
    View3D::IRenderSurface* m_activeView = nullptr;
    QList<View3D::IRenderSurface*> m_views;
    struct ViewCell {
        QWidget* cell = nullptr;
        QLabel* readout = nullptr;
        View3D::PickListenerId hoverToken = 0;
    };
    QHash<View3D::IRenderSurface*, ViewCell> m_cells;
    View3D::ViewportLayout m_layout = View3D::ViewportLayout::Single;
    View3D::ViewportSettings m_settings;
    View3D::ViewportCameraState m_lastCameraState;
    bool m_hasLastCameraState = false;
    // 进入二维前主视口的三维姿态：二维期间新建的视口继承它，退出后风格一致。
    View3D::ViewportCameraState m_planarBaseline;
    bool m_planarBaselineValid = false;
    bool m_cameraSync = false;
    bool m_initialized = false;
};

} // namespace View3DPlugins
