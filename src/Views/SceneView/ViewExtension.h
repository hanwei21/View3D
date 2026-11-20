#pragma once

#include <CommandStatePublisher.h>
#include <IApplication.h>
#include <IExtension.h>
#include <IRenderSurface.h>
#include <IViewCommandService.h>
#include <IViewportLayout.h>
#include <TileCacheSettings.h>

#include <QString>

#include <functional>
#include <QVector>

class QLabel;
class QMainWindow;

namespace View3DPlugins {

class ViewExtension final : public RF::PluginEngine::IExtension,
                            public RF::PluginEngine::IViewCommandService,
                            public RF::PluginEngine::CommandStatePublisher,
                            public View3D::ITileCacheConfig {
  public:
    bool onCreate(RF::PluginEngine::IApplication* application) override
    {
        return application && configure(application) && start(application);
    }
    void onDestroy() override { stop(); }
    bool configure(RF::PluginEngine::IApplication* runtime);
    bool start(RF::PluginEngine::IApplication* runtime);
    void stop();
    bool commandChecked(const QString& id) const override;
    bool commandEnabled(const QString& id) const override;
    void setStandardView(View3D::StandardCameraView view) override;
    void setDisplayMode(View3D::ViewportDisplayMode mode) override;
    void setBackgroundMode(View3D::ViewportBackgroundMode mode) override;
    void setGridVisible(bool visible) override;
    void setAxesVisible(bool visible) override;
    void setAmbientLightVisible(bool visible) override;
    void setMainLightVisible(bool visible) override;
    void setShadowsEnabled(bool enabled) override;
    void setShadowQuality(View3D::ViewportShadowQuality quality) override;
    void setProjectionMode(View3D::CameraProjectionMode mode) override;
    void setPlanarMode(bool enabled) override;
    void setGlobeMode(bool enabled) override;
    bool setViewportLayout(int layout) override;
    bool setCameraSync(bool enabled) override;
    void captureViewport() override;
    void showNavigationSettings() override;
    void setNavigationHelpVisible(bool visible) override;
    void setCameraStatusVisible(bool visible) override;
    void copyCameraState() override;
    void locateCameraExactly() override;
    void pasteCameraState() override;
    bool isFullscreen() const override;
    void setFullscreen(bool enabled) override;
    void setQualityPreset(int preset) override;
    // 流式瓦片缓存参数：由 system_config.xml 的 <tilecache> 决定，插件只读消费。
    View3D::TileCacheSettings tileCacheSettings() const override { return m_tileCache; }

  private:
    View3D::IRenderSurface* currentView() const;
    void applyToViews(const std::function<void(View3D::IRenderSurface*)>& action);
    // 二维状态取自视口本身：命令勾选不额外存一份状态，避免两处不一致。
    bool planarModeActive() const;
    bool globeModeActive() const;
    // 用户切换过的视图模式记在 layout_state.ini，优先于 system_config.xml 的默认值。
    void applyStoredViewMode();
    void publishStates();
    static QString serializeCameraState(const View3D::ViewportCameraState& state);
    static bool parseCameraState(const QString& text, View3D::ViewportCameraState* state);

    RF::PluginEngine::IApplication* m_application = nullptr;
    QMainWindow* m_window = nullptr;
    View3D::IViewportLayout* m_layout = nullptr;
    View3D::IRenderSurface* m_viewport = nullptr;
    QLabel* m_navigationHelp = nullptr;
    QLabel* m_cameraStatus = nullptr;
    View3D::TileCacheSettings m_tileCache;
};

} // namespace View3DPlugins
