#include "MeasureSession.h"

#include "MeasureDock.h"
#include "MeasurementMath.h"

#include <IApplication.h>
#include <IMessageCenter.h>
#include <IPluginContainer.h>
#include <IUiObjectRegistry.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <IViewportLayout.h>
#include <View3DObjectIds.h>
#include <ViewportPickResult.h>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

namespace View3DPlugins {

MeasureSession::MeasureSession(RF::PluginEngine::IApplication* application, QObject* parent)
    : QObject(parent), m_application(application)
{
}

MeasureSession::~MeasureSession()
{
    stop();
}

bool MeasureSession::start()
{
    if (!m_application)
        return false;
    auto* container = m_application->getPluginContainer();
    if (!container)
        return false;
    m_layout = container->objectAs<View3D::IViewportLayout>(
        View3D::ServiceIds::ViewportLayout);
    if (!m_layout)
        return false;
    m_layoutGuard = dynamic_cast<QObject*>(m_layout);
    if (!m_layoutGuard)
        return false;

    m_menuRegistry = container->objectAs<View3D::IViewportMenuRegistry>(
        View3D::ServiceIds::ViewportMenu);
    if (m_menuRegistry) {
        m_menuRegistry->addViewportMenuContributor(this);
        m_registeredContributor = true;
    }
    auto* ui = m_application->getUiObjectRegistry();
    m_viewportHost = ui ? ui->widget(QStringLiteral("view.document_main")) : nullptr;
    m_dockWidget = ui ? ui->widget(QStringLiteral("dw.measurements")) : nullptr;
    m_dock = dynamic_cast<MeasureDock*>(
        container->getDockWindows().value(QStringLiteral("dw.measurements"), nullptr));
    if (m_dock)
        m_dock->setSession(this);

    installPickCallbacks();
    ensureGlobeListener();
    if (qApp) {
        qApp->installEventFilter(this);
        m_appFilterInstalled = true;
    }
    if (m_viewportHost)
        m_viewportHost->installEventFilter(this);

    refreshDock();
    refreshHint();
    publishStates();
    std::cerr << "VIEW3D_MEASURE_READY layout_views=" << views().size() << std::endl;
    return true;
}

void MeasureSession::stop()
{
    deactivateTool();
    if (m_globeToken != 0) {
        if (auto* scene = renderScene())
            scene->removeGlobeModeChangeListener(m_globeToken);
        m_globeToken = 0;
    }
    if (m_registeredContributor && m_menuRegistry)
        m_menuRegistry->removeViewportMenuContributor(this);
    m_registeredContributor = false;
    m_menuRegistry = nullptr;
    if (m_appFilterInstalled && qApp)
        qApp->removeEventFilter(this);
    m_appFilterInstalled = false;
    if (m_viewportHost)
        m_viewportHost->removeEventFilter(this);
    if (m_layoutGuard)
        clearPickCallbacks();
    if (m_dock && m_dockWidget)
        m_dock->setSession(nullptr);
    m_dock = nullptr;
    m_dockWidget = nullptr;
    m_viewportHost = nullptr;
    m_layout = nullptr;
    m_layoutGuard = nullptr;
    m_pending.clear();
    m_results.clear();
    m_displayRow = -1;
}

void MeasureSession::setMeasureTool(MeasureTool tool, bool active)
{
    if (active)
        activateTool(tool);
    else if (m_tool == tool)
        deactivateTool();
}

void MeasureSession::activateTool(MeasureTool tool)
{
    if (tool == MeasureTool::None) {
        deactivateTool();
        return;
    }
    if (m_tool == tool)
        return;
    if (m_tool != MeasureTool::None)
        deactivateTool();
    m_tool = tool;
    m_pending.clear();
    m_displayRow = -1;
    m_transientHint.clear();
    if (!m_cursorOverridden) {
        QApplication::setOverrideCursor(Qt::CrossCursor);
        m_cursorOverridden = true;
    }
    installPickCallbacks();
    refreshOverlays();
    refreshDock();
    refreshHint();
    publishStates();
}

void MeasureSession::deactivateTool()
{
    const bool hadTool = m_tool != MeasureTool::None;
    m_tool = MeasureTool::None;
    m_pending.clear();
    m_displayRow = -1;
    m_transientHint.clear();
    if (m_cursorOverridden) {
        QApplication::restoreOverrideCursor();
        m_cursorOverridden = false;
    }
    refreshOverlays();
    refreshDock();
    refreshHint();
    if (hadTool)
        publishStates();
}

void MeasureSession::setSnappingEnabled(bool enabled)
{
    m_snapping = enabled;
    refreshHint();
    publishStates();
}

bool MeasureSession::measureToolActive(MeasureTool tool) const
{
    return m_tool == tool;
}

void MeasureSession::installPickCallbacks()
{
    if (!m_layout || !m_layoutGuard)
        return;
    const auto liveViews = m_layout->views();
    // 视口销毁时它的监听者随对象一起消失，这里只丢掉已经不在布局里的登记。
    for (auto it = m_pickListeners.begin(); it != m_pickListeners.end();) {
        if (liveViews.contains(it.key()))
            ++it;
        else
            it = m_pickListeners.erase(it);
    }
    for (View3D::IRenderSurface* view : liveViews) {
        if (!view || m_pickListeners.contains(view))
            continue;
        const View3D::PickListenerId id = view->addPickListener(
            [this, view](const View3D::ViewportPickResult& pick) {
                handlePick(view, pick);
            });
        if (id != 0)
            m_pickListeners.insert(view, id);
    }
}

void MeasureSession::clearPickCallbacks()
{
    if (m_layout && m_layoutGuard) {
        for (auto it = m_pickListeners.cbegin(); it != m_pickListeners.cend(); ++it) {
            if (it.key())
                it.key()->removePickListener(it.value());
        }
    }
    m_pickListeners.clear();
}

View3D::IRenderScene* MeasureSession::renderScene() const
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    return container ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
                     : nullptr;
}

void MeasureSession::ensureGlobeListener()
{
    if (m_globeToken != 0)
        return;
    auto* scene = renderScene();
    if (!scene)
        return;
    m_globeToken = scene->addGlobeModeChangeListener([this](bool enabled) {
        // 局部坐标与 ECEF 没有互换意义：切换模式后旧点/旧结果统一作废。
        m_pending.clear();
        m_results.clear();
        m_displayRow = -1;
        refreshOverlays();
        refreshDock();
        refreshHint();
        std::cerr << "VIEW3D_MEASURE_GLOBE_CLEARED mode=" << (enabled ? 1 : 0)
                  << std::endl;
    });
}

MeasureAxes MeasureSession::axesFor(View3D::IRenderSurface* view) const
{
    View3D::IRenderSurface* target = view;
    if (!target && m_layout)
        target = m_layout->activeView() ? m_layout->activeView() : m_layout->primaryView();
    if (auto* scene = renderScene(); scene && scene->globeModeEnabled())
        return MeasureAxes::Globe;
    return (target && target->planarMode()) ? MeasureAxes::Planar : MeasureAxes::Spatial3D;
}

void MeasureSession::handlePick(View3D::IRenderSurface* view,
                                const View3D::ViewportPickResult& pick)
{
    if (m_tool == MeasureTool::None)
        return;
    ensureGlobeListener();   // 场景服务可能晚于本会话就绪，用到时再补一次订阅
    const MeasureAxes axes = axesFor(view);
    if (!pick.exactTriangle || !pick.hasWorldPosition) {
        setTransientHint(QStringLiteral("测量未命中模型表面，请重新拾取"));
        return;
    }
    MeasurePoint point;
    point.position = pick.worldPosition;
    point = snapToGeometry(point, pick.triangleVertices, 0.05,
                           m_snapping && pick.hasTriangleVertices, axes);
    if (axes == MeasureAxes::Globe) {
        // 球面：拾取给的是渲染空间（ECEF−锚点）；测量点按 ECEF 保存，
        // 相机移动（锚点变化）后叠加层仍贴在同一点上。
        std::array<double, 3> anchor {};
        auto* scene = renderScene();
        if (!scene || !scene->globeRenderOrigin(&anchor)) {
            setTransientHint(QStringLiteral("球面模式尚未就绪，无法测量"));
            return;
        }
        for (int axis = 0; axis < 3; ++axis)
            point.position[axis] += anchor[axis];
    }
    m_transientHint.clear();

    switch (m_tool) {
    case MeasureTool::Coordinate:
        appendResult(coordinateResult(point, axes));
        break;
    case MeasureTool::Distance:
        m_pending.append(point);
        if (m_pending.size() >= 2) {
            const MeasurePoint first = m_pending.at(0);
            const MeasurePoint second = m_pending.at(1);
            m_pending.clear();
            appendResult(distanceResult(first, second, axes));
        } else {
            refreshOverlays();
            refreshHint();
        }
        break;
    case MeasureTool::Angle:
        m_pending.append(point);
        if (m_pending.size() >= 3) {
            const MeasurePoint first = m_pending.at(0);
            const MeasurePoint vertex = m_pending.at(1);
            const MeasurePoint third = m_pending.at(2);
            m_pending.clear();
            appendResult(angleResult(first, vertex, third, axes));
        } else {
            refreshOverlays();
            refreshHint();
        }
        break;
    case MeasureTool::Polyline:
    case MeasureTool::Area:
        m_pending.append(point);
        refreshOverlays();
        refreshHint();
        break;
    default:
        break;
    }
}

void MeasureSession::completeCurrent()
{
    const MeasureAxes axes = axesFor(nullptr);
    if (m_tool == MeasureTool::Polyline) {
        if (m_pending.size() >= 2) {
            const QList<MeasurePoint> points = m_pending;
            appendResult(polylineResult(points, axes));
        }
    } else if (m_tool == MeasureTool::Area) {
        if (m_pending.size() >= 3) {
            const QList<MeasurePoint> points = m_pending;
            appendResult(areaResult(points, axes));
        }
    }
}

void MeasureSession::appendResult(MeasureResult result)
{
    if (!result.valid) {
        if (!result.summary.isEmpty())
            setTransientHint(result.summary);
        return;
    }
    m_results.append(std::move(result));
    m_pending.clear();
    m_displayRow = m_results.size() - 1;
    refreshDock();
    refreshOverlays();
    refreshHint();
    if (m_dockWidget)
        m_dockWidget->setVisible(true);
}

void MeasureSession::displayResult(int row, bool toggle)
{
    if (row < -1 || row >= m_results.size())
        return;
    if (row == -1)
        m_displayRow = -1;
    else if (toggle && m_displayRow == row)
        m_displayRow = -1;
    else
        m_displayRow = row;
    refreshDock();
    refreshOverlays();
}

void MeasureSession::locateResult(int row)
{
    if (row < 0 || row >= m_results.size() || m_results[row].points.isEmpty())
        return;
    View3D::IRenderSurface* view = activeView();
    if (!view)
        return;
    View3D::ViewportCameraState state;
    if (!view->cameraState(&state))
        return;
    const std::array<double, 3>& point = m_results[row].points.first().position;
    double distance = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double delta = state.position[axis] - state.target[axis];
        distance += delta * delta;
    }
    distance = std::sqrt(distance);
    if (distance < 1e-6)
        distance = 10.0;
    state.target = point;
    for (int axis = 0; axis < 3; ++axis)
        state.position[axis] = point[axis] - state.direction[axis] * distance;
    view->setCameraState(state);
}

void MeasureSession::removeResult(int row)
{
    if (row < 0 || row >= m_results.size())
        return;
    m_results.removeAt(row);
    if (m_displayRow == row)
        m_displayRow = -1;
    else if (m_displayRow > row)
        --m_displayRow;
    refreshDock();
    refreshOverlays();
}

void MeasureSession::clearResults()
{
    m_results.clear();
    m_pending.clear();
    m_displayRow = -1;
    refreshDock();
    refreshOverlays();
    refreshHint();
}

void MeasureSession::appendViewportMenu(const View3D::ViewportMenuContext& context,
                                        QList<View3D::ViewportMenuAction>* actions)
{
    (void)context;
    if (!actions || m_tool == MeasureTool::None)
        return;
    bool added = false;
    if (m_tool == MeasureTool::Polyline && m_pending.size() >= 2) {
        View3D::ViewportMenuAction finish;
        finish.text =
            QStringLiteral("完成折线（已拾取 %1 个点）").arg(m_pending.size());
        finish.icon = QStringLiteral("action-finish");
        finish.triggered = [this] { completeCurrent(); };
        actions->append(finish);
        added = true;
    } else if (m_tool == MeasureTool::Area && m_pending.size() >= 3) {
        View3D::ViewportMenuAction finish;
        finish.text =
            QStringLiteral("完成多边形（已拾取 %1 个顶点）").arg(m_pending.size());
        finish.icon = QStringLiteral("action-finish");
        finish.triggered = [this] { completeCurrent(); };
        actions->append(finish);
        added = true;
    }
    View3D::ViewportMenuAction exit;
    exit.text = QStringLiteral("退出「%1」（Esc）").arg(measureToolCaption(m_tool));
    exit.icon = QStringLiteral("action-exit");
    exit.separatorBefore = added;
    exit.triggered = [this] { deactivateTool(); };
    actions->append(exit);
}

bool MeasureSession::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && m_tool != MeasureTool::None &&
            !QApplication::activeModalWidget()) {
            deactivateTool();
            return true;
        }
    } else if (event->type() == QEvent::ChildAdded && watched == m_viewportHost.data()) {
        QTimer::singleShot(0, this, [this] {
            if (m_layoutGuard)
                installPickCallbacks();
        });
    }
    return QObject::eventFilter(watched, event);
}

void MeasureSession::refreshOverlays()
{
    if (!m_layout || !m_layoutGuard)
        return;
    const QList<MeasurePoint>* source = nullptr;
    bool closed = false;
    if (!m_pending.isEmpty()) {
        source = &m_pending;
        closed = m_tool == MeasureTool::Area && m_pending.size() >= 3;
    } else if (m_displayRow >= 0 && m_displayRow < m_results.size()) {
        source = &m_results[m_displayRow].points;
        closed = m_results[m_displayRow].closedPolygon;
    }
    std::vector<std::array<double, 3>> points;
    if (source) {
        points.reserve(static_cast<std::size_t>(source->size()) + (closed ? 1 : 0));
        for (const MeasurePoint& point : *source)
            points.push_back(point.position);
        if (closed && points.size() >= 3)
            points.push_back(points.front());
    }
    for (View3D::IRenderSurface* view : m_layout->views()) {
        if (!view)
            continue;
        if (points.empty())
            view->clearOverlay();
        else
            view->setOverlay(points, points.size() > 1);
    }
}

void MeasureSession::refreshDock()
{
    if (!m_dock || !m_dockWidget)
        return;
    QList<QStringList> rows;
    rows.reserve(m_results.size());
    for (int index = 0; index < m_results.size(); ++index) {
        const MeasureResult& result = m_results[index];
        QStringList coordinates;
        for (const MeasurePoint& point : result.points)
            coordinates << formatMeasurePoint(point, result.axes);
        rows.append({QStringLiteral("%1 %2").arg(result.typeName).arg(index + 1),
                     result.summary, coordinates.join(QStringLiteral(" → "))});
    }
    m_dock->setRows(rows, m_displayRow);
}

void MeasureSession::refreshHint()
{
    if (!m_dock || !m_dockWidget)
        return;
    m_dock->setHint(currentHint());
}

void MeasureSession::setTransientHint(const QString& text)
{
    m_transientHint = text;
    refreshHint();
}

QString MeasureSession::currentHint() const
{
    if (!m_transientHint.isEmpty())
        return m_transientHint;
    if (m_tool == MeasureTool::None) {
        return QStringLiteral(
            "未激活测量工具。在“分析→基础测量”中选择工具后，左键拾取模型表面。");
    }
    QString hint =
        QStringLiteral("【%1】%2").arg(measureToolCaption(m_tool), measureUsageHint(m_tool));
    if (m_tool == MeasureTool::Polyline && !m_pending.isEmpty())
        hint += QStringLiteral(" 已拾取 %1 个点。").arg(m_pending.size());
    else if (m_tool == MeasureTool::Area && !m_pending.isEmpty())
        hint += QStringLiteral(" 已拾取 %1 个顶点。").arg(m_pending.size());
    return hint;
}

void MeasureSession::publishStates()
{
    if (m_application && m_application->getMessageCenter())
        m_application->getMessageCenter()->publish(
            View3D::MessageTopics::RefreshCommandStates);
}

QList<View3D::IRenderSurface*> MeasureSession::views() const
{
    if (!m_layout || !m_layoutGuard)
        return {};
    return m_layout->views();
}

View3D::IRenderSurface* MeasureSession::activeView() const
{
    if (!m_layout || !m_layoutGuard)
        return nullptr;
    if (View3D::IRenderSurface* view = m_layout->activeView())
        return view;
    return m_layout->primaryView();
}

} // namespace View3DPlugins
