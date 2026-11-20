#include <IContentsView.h>
#include <View3DObjectIds.h>
#include <IApplication.h>
#include <ICommand.h>
#include <IPluginContainer.h>
#include <IViewCommandService.h>
#include <RenderTypes.h>
#include "ViewportContainer.h"
#include <PluginFactoryRegistry.h>

namespace {
using namespace RF::PluginEngine;
class ViewCommandSupport {
  public:
    ViewCommandSupport(QString name, QString caption)
        : m_name(std::move(name)), m_caption(std::move(caption)) {
    }
    QString getCaption() const {
        return m_caption;
    }
    QString getCategory() const {
        return QStringLiteral("view");
    }
    bool getChecked() const {
        return module() && module()->commandChecked(m_name);
    }
    bool getEnabled() const {
        return module() && module()->commandEnabled(m_name);
    }
    QString getMessage() const {
        return {};
    }
    QString getName() const {
        return m_name;
    }
    QString getTooltip() const {
        return {};
    }
    void onCreate(RF::PluginEngine::IApplication *a) {
        app = a;
    }
    void onDestroy() {
        app = nullptr;
    }

  public:
    RF::PluginEngine::IViewCommandService *module() const {
        auto *objects = app ? app->getPluginContainer() : nullptr;
        return objects ? objects->objectAs<RF::PluginEngine::IViewCommandService>(
                              View3D::ServiceIds::ViewCommands)
                        : nullptr;
    }

  private:
    QString m_name, m_caption;
    RF::PluginEngine::IApplication *app = nullptr;
};
#define VIEW_COMMAND_MEMBERS(Name, Caption)                                                       \
    ViewCommandSupport m_support{QStringLiteral(Name), QStringLiteral(Caption)};                  \
    QString getCaption() const override { return m_support.getCaption(); }                         \
    QString getCategory() const override { return m_support.getCategory(); }                       \
    bool getChecked() const override { return m_support.getChecked(); }                            \
    bool getEnabled() const override { return m_support.getEnabled(); }                            \
    QString getMessage() const override { return m_support.getMessage(); }                         \
    QString getName() const override { return m_support.getName(); }                               \
    QString getTooltip() const override { return m_support.getTooltip(); }                         \
    void onCreate(RF::PluginEngine::IApplication *app) override { m_support.onCreate(app); }       \
    void onDestroy() override { m_support.onDestroy(); }
#define VIEW_VALUE_COMMAND(ClassName, Name, Caption, Setter, Value)                               \
    class ClassName final : public RF::PluginEngine::ICommand {                                   \
      public:                                                                                      \
        ClassName() = default;                                                                     \
        VIEW_COMMAND_MEMBERS(Name, Caption)                                                        \
        void onClick() override { if (auto *service = m_support.module()) service->Setter(Value); }\
    };
VIEW_VALUE_COMMAND(FrontViewCommand, "cmd.view.front", "前视图", setStandardView, View3D::StandardCameraView::Front)
VIEW_VALUE_COMMAND(BackViewCommand, "cmd.view.back", "后视图", setStandardView, View3D::StandardCameraView::Back)
VIEW_VALUE_COMMAND(LeftViewCommand, "cmd.view.left", "左视图", setStandardView, View3D::StandardCameraView::Left)
VIEW_VALUE_COMMAND(RightViewCommand, "cmd.view.right", "右视图", setStandardView, View3D::StandardCameraView::Right)
VIEW_VALUE_COMMAND(TopViewCommand, "cmd.view.top", "俯视图", setStandardView, View3D::StandardCameraView::Top)
VIEW_VALUE_COMMAND(BottomViewCommand, "cmd.view.bottom", "仰视图", setStandardView, View3D::StandardCameraView::Bottom)
VIEW_VALUE_COMMAND(IsometricViewCommand, "cmd.view.isometric", "轴测视图", setStandardView, View3D::StandardCameraView::Isometric)
VIEW_VALUE_COMMAND(SolidDisplayCommand, "cmd.display.solid", "实体", setDisplayMode, View3D::ViewportDisplayMode::Solid)
VIEW_VALUE_COMMAND(WireframeDisplayCommand, "cmd.display.wireframe", "线框", setDisplayMode, View3D::ViewportDisplayMode::Wireframe)
VIEW_VALUE_COMMAND(SolidWireframeDisplayCommand, "cmd.display.solid_wireframe", "实体加边线", setDisplayMode, View3D::ViewportDisplayMode::SolidWireframe)
VIEW_VALUE_COMMAND(PointsDisplayCommand, "cmd.display.points", "点模式", setDisplayMode, View3D::ViewportDisplayMode::Points)
VIEW_VALUE_COMMAND(SolidBackgroundCommand, "cmd.environment.background_solid", "单色背景", setBackgroundMode, View3D::ViewportBackgroundMode::Solid)
VIEW_VALUE_COMMAND(GradientBackgroundCommand, "cmd.environment.background_gradient", "渐变背景", setBackgroundMode, View3D::ViewportBackgroundMode::Gradient)
VIEW_VALUE_COMMAND(PerformanceShadowCommand, "cmd.environment.shadow_quality_performance", "阴影-性能", setShadowQuality, View3D::ViewportShadowQuality::Performance)
VIEW_VALUE_COMMAND(BalancedShadowCommand, "cmd.environment.shadow_quality_balanced", "阴影-平衡", setShadowQuality, View3D::ViewportShadowQuality::Balanced)
VIEW_VALUE_COMMAND(QualityShadowCommand, "cmd.environment.shadow_quality_quality", "阴影-质量", setShadowQuality, View3D::ViewportShadowQuality::Quality)
VIEW_VALUE_COMMAND(QualityPerformancePresetCommand, "cmd.quality.performance", "质量-性能", setQualityPreset, 0)
VIEW_VALUE_COMMAND(QualityBalancedPresetCommand, "cmd.quality.balanced", "质量-平衡", setQualityPreset, 1)
VIEW_VALUE_COMMAND(QualityHighPresetCommand, "cmd.quality.quality", "质量-质量", setQualityPreset, 2)
VIEW_VALUE_COMMAND(PerspectiveProjectionCommand, "cmd.projection.perspective", "透视投影", setProjectionMode, View3D::CameraProjectionMode::Perspective)
VIEW_VALUE_COMMAND(OrthographicProjectionCommand, "cmd.projection.orthographic", "正交投影", setProjectionMode, View3D::CameraProjectionMode::Orthographic)
VIEW_VALUE_COMMAND(PlanarMode2DCommand, "cmd.view.mode_2d", "二维模式", setPlanarMode, true)
VIEW_VALUE_COMMAND(PlanarMode3DCommand, "cmd.view.mode_3d", "三维模式", setPlanarMode, false)
VIEW_VALUE_COMMAND(GlobeModeCommand, "cmd.view.mode_globe", "球面模式", setGlobeMode, true)
VIEW_VALUE_COMMAND(SingleLayoutCommand, "cmd.viewport.layout_single", "单视口", setViewportLayout, 0)
VIEW_VALUE_COMMAND(DualLayoutCommand, "cmd.viewport.layout_dual", "双视口", setViewportLayout, 1)
VIEW_VALUE_COMMAND(QuadLayoutCommand, "cmd.viewport.layout_quad", "四视口", setViewportLayout, 2)
#undef VIEW_VALUE_COMMAND
#define VIEW_ACTION(ClassName, Name, Caption, Action)                                              \
    class ClassName final : public RF::PluginEngine::ICommand {                                   \
      public:                                                                                      \
        ClassName() = default;                                                                     \
        VIEW_COMMAND_MEMBERS(Name, Caption)                                                        \
        void onClick() override {                                                                  \
            if (auto *service = m_support.module()) service->Action();                             \
        }                                                                                          \
    };
VIEW_ACTION(CaptureViewportCommand, "cmd.view.capture", "保存截图", captureViewport)
VIEW_ACTION(NavigationSettingsCommand, "cmd.navigation.settings", "导航设置",
            showNavigationSettings)
VIEW_ACTION(CopyCameraCommand, "cmd.camera.copy", "复制相机状态", copyCameraState)
VIEW_ACTION(LocateCameraCommand, "cmd.camera.locate", "精确定位相机", locateCameraExactly)
VIEW_ACTION(PasteCameraCommand, "cmd.camera.paste", "粘贴相机状态", pasteCameraState)
#undef VIEW_ACTION
#define VIEW_TOGGLE(ClassName, Name, Caption, Action)                                              \
    class ClassName final : public RF::PluginEngine::ICommand {                                   \
      public:                                                                                      \
        ClassName() = default;                                                                     \
        VIEW_COMMAND_MEMBERS(Name, Caption)                                                        \
        void onClick() override {                                                                  \
            if (auto *service = m_support.module()) service->Action(!getChecked());                \
        }                                                                                          \
    };
VIEW_TOGGLE(GridCommand, "cmd.environment.grid", "地面网格", setGridVisible)
VIEW_TOGGLE(CameraSyncCommand, "cmd.viewport.camera_sync", "同步相机", setCameraSync)
VIEW_TOGGLE(AxesCommand, "cmd.environment.axes", "坐标轴", setAxesVisible)
VIEW_TOGGLE(AmbientLightCommand, "cmd.environment.ambient_light", "环境光", setAmbientLightVisible)
VIEW_TOGGLE(MainLightCommand, "cmd.environment.main_light", "主方向光", setMainLightVisible)
VIEW_TOGGLE(ShadowsCommand, "cmd.environment.shadows", "阴影", setShadowsEnabled)
VIEW_TOGGLE(NavigationHelpCommand, "cmd.navigation.help", "导航帮助", setNavigationHelpVisible)
VIEW_TOGGLE(CameraStatusCommand, "cmd.camera.status", "相机状态", setCameraStatusVisible)
VIEW_TOGGLE(FullscreenCommand, "cmd.view.fullscreen", "全屏", setFullscreen)
#undef VIEW_TOGGLE
RF::PluginEngine::IPlugin *createViewCommand(const RF::PluginEngine::PluginCreationContext &c) {
    using namespace RF::PluginEngine;
    const auto id = c.attributes.value(QStringLiteral("name"));
#define MATCH(Id, Expression)                                                                      \
    if (id == QStringLiteral(Id))                                                                  \
        return new Expression;
    MATCH("cmd.view.front", FrontViewCommand())
    MATCH("cmd.view.back", BackViewCommand())
    MATCH("cmd.view.left", LeftViewCommand())
    MATCH("cmd.view.right", RightViewCommand())
    MATCH("cmd.view.top", TopViewCommand())
    MATCH("cmd.view.bottom", BottomViewCommand())
    MATCH("cmd.view.isometric", IsometricViewCommand())
    MATCH("cmd.view.capture", CaptureViewportCommand())
    MATCH("cmd.display.solid", SolidDisplayCommand())
    MATCH("cmd.display.wireframe", WireframeDisplayCommand())
    MATCH("cmd.display.solid_wireframe", SolidWireframeDisplayCommand())
    MATCH("cmd.display.points", PointsDisplayCommand())
    MATCH("cmd.environment.background_solid", SolidBackgroundCommand())
    MATCH("cmd.environment.background_gradient", GradientBackgroundCommand())
    MATCH("cmd.environment.grid", GridCommand())
    MATCH("cmd.environment.axes", AxesCommand())
    MATCH("cmd.environment.ambient_light", AmbientLightCommand())
    MATCH("cmd.environment.main_light", MainLightCommand())
    MATCH("cmd.environment.shadows", ShadowsCommand())
    MATCH("cmd.environment.shadow_quality_performance", PerformanceShadowCommand())
    MATCH("cmd.environment.shadow_quality_balanced", BalancedShadowCommand())
    MATCH("cmd.environment.shadow_quality_quality", QualityShadowCommand())
    MATCH("cmd.quality.performance", QualityPerformancePresetCommand())
    MATCH("cmd.quality.balanced", QualityBalancedPresetCommand())
    MATCH("cmd.quality.quality", QualityHighPresetCommand())
    MATCH("cmd.projection.perspective", PerspectiveProjectionCommand())
    MATCH("cmd.projection.orthographic", OrthographicProjectionCommand())
    MATCH("cmd.view.mode_2d", PlanarMode2DCommand())
    MATCH("cmd.view.mode_3d", PlanarMode3DCommand())
    MATCH("cmd.view.mode_globe", GlobeModeCommand())
    MATCH("cmd.viewport.layout_single", SingleLayoutCommand())
    MATCH("cmd.viewport.layout_dual", DualLayoutCommand())
    MATCH("cmd.viewport.layout_quad", QuadLayoutCommand())
    MATCH("cmd.viewport.camera_sync", CameraSyncCommand())
    MATCH("cmd.navigation.settings", NavigationSettingsCommand())
    MATCH("cmd.navigation.help", NavigationHelpCommand())
    MATCH("cmd.camera.status", CameraStatusCommand())
    MATCH("cmd.camera.copy", CopyCameraCommand())
    MATCH("cmd.camera.locate", LocateCameraCommand())
    MATCH("cmd.camera.paste", PasteCameraCommand())
    MATCH("cmd.view.fullscreen", FullscreenCommand())
#undef MATCH
#undef VIEW_COMMAND_MEMBERS
    return nullptr;
}

class ViewportContentsView final : public RF::PluginEngine::IContentsView {
  public:
    explicit ViewportContentsView(RF::PluginEngine::PluginCreationContext context)
        : m_context(std::move(context)),
          m_container(new View3DPlugins::ViewportContainer(m_context.container)) {
    }

    ~ViewportContentsView() override = default;

    QString getCaption() const override {
        return m_context.attributes.value(QStringLiteral("text"), QStringLiteral("三维视图"));
    }
    bool getControlBox() const override {
        return true;
    }
    bool getInitCreate() const override {
        return true;
    }
    QString getName() const override {
        return m_context.attributes.value(QStringLiteral("name"));
    }
    void onActive(bool) override {
    }
    void onClose() override {
        onActive(false);
    }
    void onDestroy() override {
        if (m_destroyed)
            return;
        m_destroyed = true;
    }
    void onCreate(RF::PluginEngine::IApplication *) override {
    }
    QWidget *getWidget() const override {
        return m_container;
    }

  private:
    RF::PluginEngine::PluginCreationContext m_context;
    View3DPlugins::ViewportContainer *m_container = nullptr;
    bool m_destroyed = false;
};

} // namespace

void registerViewportContentsViewFactory(RF::PluginEngine::PluginFactoryRegistry *registry) {
    for (const QString &type : {QStringLiteral("command"), QStringLiteral("checkcommand")})
        registry->registerCreator(QStringLiteral("plugin.view3d.sceneview"),
                                  QStringLiteral("factory.create_command"), type,
                                  createViewCommand);
    registry->registerCreator(
        QStringLiteral("plugin.view3d.sceneview"), QStringLiteral("factory.create_contentsview"),
        QStringLiteral("contentsview"), [](const RF::PluginEngine::PluginCreationContext &context) {
            return new ViewportContentsView(context);
        });
}
