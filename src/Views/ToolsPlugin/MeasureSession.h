#pragma once

#include "MeasurementTypes.h"

#include <IViewportMenu.h>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QEvent;
class QWidget;

namespace View3D {
class IRenderScene;
class IRenderSurface;
class IViewportLayout;
struct ViewportPickResult;
} // namespace View3D

namespace RF::PluginEngine {
class IApplication;
}

namespace View3DPlugins {

class MeasureDock;

// 基础测量的工具会话：唯一活动工具、拾取路由、临时点、结果集、叠加层、
// 结果面板与视口右键菜单项。所有具体测量行为都在本插件内，SceneView 只
// 通过 IViewportMenuContributor 拿到菜单项。
class MeasureSession final : public QObject,
                             public IMeasureToolService,
                             public View3D::IViewportMenuContributor {
  public:
    explicit MeasureSession(RF::PluginEngine::IApplication* application,
                            QObject* parent = nullptr);
    ~MeasureSession() override;

    bool start();
    void stop();

    void setMeasureTool(MeasureTool tool, bool active) override;
    void setSnappingEnabled(bool enabled) override;
    bool measureToolActive(MeasureTool tool) const override;
    bool snappingEnabled() const override { return m_snapping; }

    void appendViewportMenu(const View3D::ViewportMenuContext& context,
                            QList<View3D::ViewportMenuAction>* actions) override;

    const QList<MeasureResult>& results() const { return m_results; }
    int displayedRow() const { return m_displayRow; }
    void displayResult(int row, bool toggle);
    void locateResult(int row);
    void removeResult(int row);
    void clearResults();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void activateTool(MeasureTool tool);
    void deactivateTool();
    void installPickCallbacks();
    void clearPickCallbacks();
    // 球面切换会换坐标空间（局部 ↔ ECEF）：已有点/结果在另一空间里没有意义，
    // 统一清空并刷新叠加层。
    void ensureGlobeListener();
    View3D::IRenderScene* renderScene() const;
    void handlePick(View3D::IRenderSurface* view,
                    const View3D::ViewportPickResult& pick);
    // 二维视口里的测量只用水平面两轴；view 为空时取当前活动视口。
    MeasureAxes axesFor(View3D::IRenderSurface* view) const;
    void completeCurrent();
    void appendResult(MeasureResult result);
    void refreshOverlays();
    void refreshDock();
    void refreshHint();
    void publishStates();
    void setTransientHint(const QString& text);
    QString currentHint() const;
    QList<View3D::IRenderSurface*> views() const;
    View3D::IRenderSurface* activeView() const;

    RF::PluginEngine::IApplication* m_application = nullptr;
    View3D::IViewportLayout* m_layout = nullptr;
    QPointer<QObject> m_layoutGuard;
    View3D::IViewportMenuRegistry* m_menuRegistry = nullptr;
    QPointer<QWidget> m_viewportHost;
    QPointer<QWidget> m_dockWidget;
    MeasureDock* m_dock = nullptr;

    MeasureTool m_tool = MeasureTool::None;
    bool m_snapping = true;
    bool m_cursorOverridden = false;
    QList<MeasurePoint> m_pending;
    QList<MeasureResult> m_results;
    // 视口 -> 本会话的拾取监听者 id（View3D::PickListenerId 就是 quint64）。
    // 拾取是按监听者广播的，注销自己的登记不会动到别的插件（如要素属性表）。
    QHash<View3D::IRenderSurface*, quint64> m_pickListeners;
    int m_displayRow = -1;
    QString m_transientHint;
    bool m_registeredContributor = false;
    bool m_appFilterInstalled = false;
    std::uint64_t m_globeToken = 0;
};

} // namespace View3DPlugins
