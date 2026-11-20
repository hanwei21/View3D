#include "ViewportContainer.h"

#include "SceneLayerService.h"

#include <GlobeMath.h>
#include <IGeoReference.h>
#include <IPluginContainer.h>
#include <IViewportMenu.h>
#include <View3DObjectIds.h>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QGridLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLayoutItem>
#include <QMenu>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <functional>
#include <utility>

namespace View3DPlugins {
namespace {

QIcon uiIcon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    const auto found = cache.constFind(name);
    if (found != cache.constEnd())
        return found.value();
    const QIcon icon(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("resources/icons/%1.svg").arg(name)));
    cache.insert(name, icon);
    return icon;
}

} // namespace

ViewportContainer::ViewportContainer(RF::PluginEngine::IPluginContainer* container,
                                     QWidget* parent)
    : QWidget(parent), m_pluginContainer(container)
{
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(1);
    m_cameraSyncTimer = new QTimer(this);
    m_cameraSyncTimer->setInterval(50);
    connect(m_cameraSyncTimer, &QTimer::timeout, this, [this] { syncCameras(); });
    if (m_pluginContainer) {
        m_pluginContainer->publishAs<View3D::IViewportLayout>(
            View3D::ServiceIds::ViewportLayout, this);
        m_viewportMenuRegistry = m_pluginContainer->objectAs<View3D::IViewportMenuRegistry>(
            View3D::ServiceIds::ViewportMenu);
    }
}

ViewportContainer::~ViewportContainer()
{
    if (m_alive)
        *m_alive = false;
    if (m_cameraSyncTimer)
        m_cameraSyncTimer->stop();
    for (auto it = m_cells.cbegin(); it != m_cells.cend(); ++it) {
        if (it.key() && it.value().hoverToken != 0)
            it.key()->removeHoverPickListener(it.value().hoverToken);
    }
    m_cells.clear();
    for (View3D::IRenderSurface* view : m_views) {
        if (m_renderService)
            m_renderService->destroyView(view);
    }
    m_views.clear();
    m_primary = nullptr;
    m_activeView = nullptr;
    m_renderService = nullptr;
    m_scene = nullptr;
    if (m_pluginContainer) {
        m_pluginContainer->unpublishObject(
            View3D::ServiceIds::ViewportLayout,
            static_cast<View3D::IViewportLayout*>(this));
    }
}

bool ViewportContainer::initialize()
{
    if (m_initialized)
        return true;
    if (!m_pluginContainer)
        return false;
    m_renderService = m_pluginContainer->objectAs<View3D::IRenderService>(
        View3D::ServiceIds::Renderer);
    m_scene = m_pluginContainer->objectAs<View3D::IRenderScene>(
        View3D::ServiceIds::Scene);
    if (!m_viewportMenuRegistry) {
        m_viewportMenuRegistry = m_pluginContainer->objectAs<View3D::IViewportMenuRegistry>(
            View3D::ServiceIds::ViewportMenu);
    }
    if (!m_renderService || !m_scene)
        return false;
    ensureViewportCount(layoutViewportCount());
    if (!m_primary)
        return false;
    m_initialized = true;
    m_activeView = m_primary;
    rebuildLayout();
    applyVisibility();
    applySettings(m_settings);
    updateCameraSyncTimer();
    return true;
}

bool ViewportContainer::setLayout(View3D::ViewportLayout layout)
{
    m_layout = layout;
    if (!m_initialized)
        return true;
    const int targetCount = layoutViewportCount();
    ensureViewportCount(targetCount);
    if (m_activeView && !m_views.mid(0, targetCount).contains(m_activeView))
        m_activeView = m_primary;
    rebuildLayout();
    applyVisibility();
    updateCameraSyncTimer();
    return true;
}

bool ViewportContainer::setActiveView(View3D::IRenderSurface* view)
{
    if (!view || !m_views.contains(view))
        return false;
    m_activeView = view;
    return true;
}

bool ViewportContainer::setCameraSync(bool enabled)
{
    m_cameraSync = enabled;
    m_hasLastCameraState = false;
    updateCameraSyncTimer();
    if (enabled)
        syncCameras();
    return true;
}

void ViewportContainer::applySettings(const View3D::ViewportSettings& settings)
{
    m_settings = settings;
    if (!m_scene)
        return;
    m_scene->setDisplayMode(settings.displayMode);
    m_scene->setEnvironmentStyle(settings.environment);
    m_scene->setLightingStyle(settings.lighting);
    m_scene->setRenderBudgetMaxVertices(settings.renderBudgetMaxVertices);
    m_renderService->setTargetFrameRate(settings.targetFrameRate);
    for (View3D::IRenderSurface* view : m_views)
        applySettingsToView(view);
    // 走同一条路径，避免 applySettings 与 setPlanarMode 各存一份模式状态。
    setPlanarMode(settings.planarMode);
    // 配置里两者同时为真时球面优先：它只显示 3D Tiles，二维会被退出。
    setGlobeModeEnabled(settings.globeMode);
}

void ViewportContainer::applySettingsToView(View3D::IRenderSurface* view)
{
    if (!view)
        return;
    view->setBackgroundStyle(m_settings.background);
    view->setShadowStyle(m_settings.shadows);
}

int ViewportContainer::layoutViewportCount() const
{
    return m_layout == View3D::ViewportLayout::Single ? 1
        : m_layout == View3D::ViewportLayout::Dual          ? 2
                                                           : 4;
}

bool ViewportContainer::setPlanarMode(bool enabled)
{
    // 与球面互斥：球面编排会把相机换回局部切平面，再继续进二维。
    if (enabled && globeModeEnabled())
        setGlobeModeEnabled(false);
    const bool wasPlanar = m_primary ? m_primary->planarMode() : m_settings.planarMode;
    // 进入二维前记下主视口的三维姿态：二维期间新建的视口用它作为还原基准，
    // 否则新视口会把自己的默认相机当成基准，退出后各视口投影/朝向不一致。
    if (enabled && !wasPlanar && m_primary)
        m_planarBaselineValid = m_primary->cameraState(&m_planarBaseline);
    bool applied = true;
    for (View3D::IRenderSurface* view : m_views) {
        if (view && !view->setPlanarMode(enabled))
            applied = false;
    }
    // 以视口的实际状态为准，避免布局记录与画面不一致。
    m_settings.planarMode = planarMode();
    if (m_scene)
        m_scene->setAxesPlanar(m_settings.planarMode);
    if (!m_settings.planarMode)
        m_planarBaselineValid = false;
    return applied;
}

bool ViewportContainer::setGlobeModeEnabled(bool enabled)
{
    if (!m_initialized || !m_scene || !m_primary)
        return false;
    if (enabled == globeModeEnabled())
        return true;
    // 与二维互斥：先退二维（它恢复自己的相机），换算基准才是平面相机。
    if (enabled && planarMode())
        setPlanarMode(false);

    ensureLayerService();
    // 先把所有视口相机准备好：进入时局部→ECEF，退出时 ECEF→局部。
    // 会话重启在场景开关内同步完成，随后这里立刻写入相机，同一线程不会穿插渲染。
    struct ViewSwitch {
        View3D::IRenderSurface* view = nullptr;
        View3D::ViewportCameraState flat;
        View3D::ViewportCameraState target;
    };
    QList<ViewSwitch> switches;
    for (View3D::IRenderSurface* view : std::as_const(m_views)) {
        if (!view)
            continue;
        ViewSwitch entry;
        entry.view = view;
        View3D::ViewportCameraState current;
        if (!view->cameraState(&current)) {
            entry.target = enabled ? defaultGlobeCameraState(current)
                                   : defaultLocalCameraState();
            switches.append(entry);
            continue;
        }
        entry.flat = current;
        entry.target = current;
        if (enabled) {
            if (!convertCameraToGlobe(current, &entry.target))
                entry.target = defaultGlobeCameraState(current);
        } else if (!convertCameraToLocal(current, &entry.target)) {
            // 没有地理参考时（空球面）无法反算：回到进入球面前的平面相机。
            if (m_globeFlatCameras.contains(view))
                entry.target = m_globeFlatCameras.value(view);
            else if (m_globeFlatCameras.contains(m_primary))
                entry.target = m_globeFlatCameras.value(m_primary);
            else
                entry.target = defaultLocalCameraState();
        }
        switches.append(entry);
    }

    if (!m_scene->setGlobeModeEnabled(enabled))
        return false;
    if (enabled) {
        for (const ViewSwitch& entry : std::as_const(switches))
            m_globeFlatCameras.insert(entry.view, entry.flat);
    }
    // 锚点跟随最后一次 setCameraState（与 prepareFrame 同一语义）：活动视口
    // 放到最后写，隐藏视口不会把锚点带偏、让首帧/离屏截图的几何相对相机偏移。
    View3D::IRenderSurface* anchorView = enabled ? m_activeView : nullptr;
    if (!anchorView || !m_views.contains(anchorView))
        anchorView = enabled ? m_primary : nullptr;
    for (const ViewSwitch& entry : std::as_const(switches)) {
        if (entry.view == anchorView)
            continue;
        // 顺序固定：视口先进球面语义，再写 ECEF 相机。
        entry.view->setGlobeModeEnabled(enabled);
        entry.view->setCameraState(entry.target);
    }
    for (const ViewSwitch& entry : std::as_const(switches)) {
        if (entry.view != anchorView)
            continue;
        entry.view->setGlobeModeEnabled(enabled);
        entry.view->setCameraState(entry.target);
    }
    setFlatOnlyLayersHidden(enabled);
    if (!enabled)
        m_globeFlatCameras.clear();
    m_settings.globeMode = enabled;
    return true;
}

bool ViewportContainer::convertCameraToGlobe(const View3D::ViewportCameraState& local,
                                             View3D::ViewportCameraState* globe) const
{
    if (!globe || !m_pluginContainer)
        return false;
    auto* reference = m_pluginContainer->objectAs<View3D::IGeoReference>(
        View3D::ServiceIds::GeoReference);
    if (!reference || !reference->ready())
        return false;
    std::array<double, 3> position {};
    std::array<double, 3> target {};
    if (!reference->localToEcefPoint(local.position, &position) ||
        !reference->localToEcefPoint(local.target, &target))
        return false;
    // 方向与 up 用同一映射的线性部分：f(p+v) − f(p) 再归一化。
    const auto rotate = [reference](const std::array<double, 3>& origin,
                                    const std::array<double, 3>& vector,
                                    std::array<double, 3>* out) {
        const std::array<double, 3> shifted = {origin[0] + vector[0], origin[1] + vector[1],
                                               origin[2] + vector[2]};
        std::array<double, 3> mappedOrigin {};
        std::array<double, 3> mappedShifted {};
        if (!reference->localToEcefPoint(origin, &mappedOrigin) ||
            !reference->localToEcefPoint(shifted, &mappedShifted))
            return false;
        const double dx = mappedShifted[0] - mappedOrigin[0];
        const double dy = mappedShifted[1] - mappedOrigin[1];
        const double dz = mappedShifted[2] - mappedOrigin[2];
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(length > 1e-9) || !std::isfinite(length))
            return false;
        *out = {dx / length, dy / length, dz / length};
        return true;
    };
    View3D::ViewportCameraState result = local;
    result.position = position;
    result.target = target;
    if (!rotate(local.position, local.direction, &result.direction) ||
        !rotate(local.position, local.up, &result.up))
        return false;
    result.orthographic = false;
    *globe = result;
    return true;
}

bool ViewportContainer::convertCameraToLocal(const View3D::ViewportCameraState& globe,
                                             View3D::ViewportCameraState* local) const
{
    if (!local || !m_pluginContainer)
        return false;
    auto* reference = m_pluginContainer->objectAs<View3D::IGeoReference>(
        View3D::ServiceIds::GeoReference);
    if (!reference || !reference->ready())
        return false;
    std::array<double, 3> position {};
    std::array<double, 3> target {};
    if (!reference->ecefToLocalPoint(globe.position, &position) ||
        !reference->ecefToLocalPoint(globe.target, &target))
        return false;
    const auto rotate = [reference](const std::array<double, 3>& origin,
                                    const std::array<double, 3>& vector,
                                    std::array<double, 3>* out) {
        const std::array<double, 3> shifted = {origin[0] + vector[0], origin[1] + vector[1],
                                               origin[2] + vector[2]};
        std::array<double, 3> mappedOrigin {};
        std::array<double, 3> mappedShifted {};
        if (!reference->ecefToLocalPoint(origin, &mappedOrigin) ||
            !reference->ecefToLocalPoint(shifted, &mappedShifted))
            return false;
        const double dx = mappedShifted[0] - mappedOrigin[0];
        const double dy = mappedShifted[1] - mappedOrigin[1];
        const double dz = mappedShifted[2] - mappedOrigin[2];
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(length > 1e-9) || !std::isfinite(length))
            return false;
        *out = {dx / length, dy / length, dz / length};
        return true;
    };
    View3D::ViewportCameraState result = globe;
    result.position = position;
    result.target = target;
    if (!rotate(globe.position, globe.direction, &result.direction) ||
        !rotate(globe.position, globe.up, &result.up))
        return false;
    *local = result;
    return true;
}

View3D::ViewportCameraState ViewportContainer::defaultGlobeCameraState(
    const View3D::ViewportCameraState& base)
{
    using namespace View3D::GlobeMath;
    // 没有任何地理数据时给一个「看向北半球」的全球视角：只用于空球面。
    const Vec3 surface = geodeticToEcef(105.0, 30.0, 0.0);
    const Vec3 normal = ecefNormal(surface);
    const Vec3 position = add(surface, scale(normal, kSemiMajorAxis * 2.4));
    const Vec3 target = geodeticToEcef(105.0, 0.0, 0.0);
    View3D::ViewportCameraState state = base;
    state.position = position;
    state.target = target;
    state.direction = normalized(sub(target, position));
    state.up = normal;
    state.orthographic = false;
    return state;
}

View3D::ViewportCameraState ViewportContainer::defaultLocalCameraState()
{
    View3D::ViewportCameraState state;
    state.position = {0.0, 1000.0, 0.0};
    state.target = {0.0, 0.0, 0.0};
    state.direction = {0.0, -1.0, 0.0};
    state.up = {0.0, 0.0, -1.0};
    return state;
}

void ViewportContainer::ensureLayerService()
{
    if (m_layerService || !m_pluginContainer)
        return;
    auto* service = m_pluginContainer->objectAs<View3D::ISceneLayerService>(
        View3D::ServiceIds::SceneLayers);
    m_layerService = dynamic_cast<SceneLayerService*>(service);
    if (!m_layerService)
        return;
    // 球面期间新加载的影像/矢量图层也要隐藏；回调可能晚于容器析构，用存活标志兜底。
    m_layerService->addChangeCallback([alive = m_alive, this](const LayerChange& change) {
        if (!alive || !*alive)
            return;
        if (change.kind != LayerChangeKind::Added)
            return;
        if (m_scene && m_scene->globeModeEnabled())
            hideFlatOnlyLayer(change.id);
    });
}

bool ViewportContainer::hideFlatOnlyLayer(const View3D::LayerId& layerId)
{
    if (!m_layerService)
        return false;
    const auto layer = m_layerService->layers().layer(layerId);
    if (!layer)
        return false;
    // 球面模式支持瓦片（3D Tiles/倾斜摄影）与模型导入（球面重烘焙）；影像与矢量
    // 仍是平面覆盖层，没有球面摆放路径，进球面时暂存可见性后隐藏。
    if (layer->type() != View3D::LayerType::Imagery &&
        layer->type() != View3D::LayerType::Feature)
        return false;
    if (!layer->isVisible())
        return false;
    if (!m_layerService->setLayerVisible(layerId, false))
        return false;
    m_globeHiddenLayers.insert(layerId);
    return true;
}

void ViewportContainer::setFlatOnlyLayersHidden(bool hidden)
{
    if (!hidden) {
        restoreFlatOnlyLayers();
        return;
    }
    if (!m_layerService)
        return;
    const std::function<void(const View3D::LayerId&)> visit =
        [this, &visit](const View3D::LayerId& layerId) {
            hideFlatOnlyLayer(layerId);
            for (const View3D::LayerId& child : m_layerService->layers().childLayers(layerId))
                visit(child);
        };
    const View3D::LayerManager& layers = m_layerService->layers();
    for (const View3D::LayerId& root : layers.rootLayers())
        visit(root);
}

void ViewportContainer::restoreFlatOnlyLayers()
{
    if (m_layerService) {
        for (const View3D::LayerId& layerId : std::as_const(m_globeHiddenLayers)) {
            if (m_layerService->layers().layer(layerId))
                m_layerService->setLayerVisible(layerId, true);
        }
    }
    m_globeHiddenLayers.clear();
}

View3D::IRenderSurface* ViewportContainer::createViewport()
{
    if (!m_renderService || !m_scene)
        return nullptr;
    auto* view = m_renderService->createView(m_scene, this);
    if (view && view->widget())
        view->widget()->installEventFilter(this);
    if (view) {
        applySettingsToView(view);
        if (m_scene && m_scene->globeModeEnabled()) {
            // 球面下新建的视口继承主视口的 ECEF 相机：默认平面相机会被当成
            // ECEF 值（落在地心附近），画面飞走。
            view->setGlobeModeEnabled(true);
            View3D::ViewportCameraState state;
            if (m_primary && m_primary->cameraState(&state))
                view->setCameraState(state);
        } else if (m_settings.planarMode) {
            // 布局切换新建的视口要继承当前二维/三维模式，否则相机同步会把它
            // 的斜视姿态倒灌给已锁定的视口。先铺上共享的三维基准再进二维，
            // 这样退出时它和其他视口回到同一种观察风格。
            if (m_planarBaselineValid)
                view->setCameraState(m_planarBaseline);
            view->setPlanarMode(true);
        }
    }
    return view;
}

void ViewportContainer::ensureViewportCount(int count)
{
    while (m_views.size() < count) {
        auto* view = createViewport();
        if (!view)
            break;
        m_views.append(view);
        if (!m_primary)
            m_primary = view;
    }
}

void ViewportContainer::syncCameras()
{
    const int targetCount = qMin(layoutViewportCount(), m_views.size());
    if (!m_cameraSync || !m_primary || targetCount < 2)
        return;
    View3D::ViewportCameraState camera;
    if (!m_primary->cameraState(&camera))
        return;
    if (m_hasLastCameraState && camera == m_lastCameraState)
        return;
    m_lastCameraState = camera;
    m_hasLastCameraState = true;
    for (int index = 1; index < targetCount; ++index) {
        if (m_views[index] != m_primary)
            m_views[index]->setCameraState(camera);
    }
}

void ViewportContainer::updateCameraSyncTimer()
{
    if (!m_cameraSyncTimer)
        return;
    if (m_cameraSync && m_initialized &&
        qMin(layoutViewportCount(), m_views.size()) >= 2) {
        if (!m_cameraSyncTimer->isActive())
            m_cameraSyncTimer->start();
    } else {
        m_cameraSyncTimer->stop();
    }
}

View3D::IRenderSurface* ViewportContainer::viewForWidget(QObject* watched) const
{
    if (!watched)
        return nullptr;
    for (View3D::IRenderSurface* view : m_views) {
        if (view && view->widget() == watched)
            return view;
    }
    return nullptr;
}

void ViewportContainer::showViewportMenu(View3D::IRenderSurface* view,
                                         const QPoint& localPosition,
                                         const QPoint& globalPosition)
{
    if (!m_viewportMenuRegistry || !view)
        return;
    QList<View3D::ViewportMenuAction> actions;
    const View3D::ViewportMenuContext context{view, localPosition};
    for (View3D::IViewportMenuContributor* contributor :
         m_viewportMenuRegistry->viewportMenuContributors()) {
        if (contributor)
            contributor->appendViewportMenu(context, &actions);
    }
    if (actions.isEmpty())
        return;
    QMenu menu(this);
    for (const View3D::ViewportMenuAction& action : std::as_const(actions)) {
        if (action.text.isEmpty() || !action.triggered)
            continue;
        if (action.separatorBefore && !menu.isEmpty())
            menu.addSeparator();
        QAction* entry = menu.addAction(action.text);
        entry->setEnabled(action.enabled);
        if (!action.icon.isEmpty())
            entry->setIcon(uiIcon(action.icon));
        QObject::connect(entry, &QAction::triggered, this,
                         [callback = action.triggered] { callback(); });
    }
    if (menu.isEmpty())
        return;
    menu.exec(globalPosition);
}

bool ViewportContainer::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        View3D::IRenderSurface* view = viewForWidget(watched);
        if (view)
            setActiveView(view);
        if (mouse->button() == Qt::RightButton && view) {
            m_rightClickArmed = true;
            m_rightClickStart = mouse->position().toPoint();
            m_rightClickView = view;
        }
        break;
    }
    case QEvent::MouseMove: {
        if (!m_rightClickArmed)
            break;
        auto* mouse = static_cast<QMouseEvent*>(event);
        if ((mouse->buttons() & Qt::RightButton) &&
            (mouse->position().toPoint() - m_rightClickStart).manhattanLength() > 5) {
            m_rightClickArmed = false;
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::RightButton || !m_rightClickArmed)
            break;
        m_rightClickArmed = false;
        View3D::IRenderSurface* view = m_rightClickView;
        m_rightClickView = nullptr;
        if (!view)
            break;
        const QPoint local = mouse->position().toPoint();
        const QPoint global = mouse->globalPosition().toPoint();
        // 右键拖拽仍是轨道旋转；只有未拖动的右键点击才弹工具菜单。排队到事件
        // 返回后再弹，避免在旋转状态尚未清空时进入嵌套事件循环。
        QTimer::singleShot(0, this, [this, view, local, global] {
            showViewportMenu(view, local, global);
        });
        break;
    }
    case QEvent::Leave:
        m_rightClickArmed = false;
        m_rightClickView = nullptr;
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

QWidget* ViewportContainer::cellFor(View3D::IRenderSurface* view)
{
    if (!view || !view->widget())
        return nullptr;
    const auto existing = m_cells.constFind(view);
    if (existing != m_cells.constEnd())
        return existing->cell;

    auto* cell = new QWidget(this);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(view->widget(), 1);
    auto* readout = new QLabel(cell);
    // 无头冒烟按 objectName 找这条读数，不依赖它在布局里的位置。
    readout->setObjectName(QStringLiteral("viewport.coordinateReadout"));
    readout->setTextFormat(Qt::PlainText);
    // 坐标要能直接选中复制；水平方向不参与布局计数，否则长坐标会把视口列撑宽。
    readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    readout->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    readout->setContentsMargins(6, 1, 6, 2);
    readout->setText(QStringLiteral("坐标 —"));
    layout->addWidget(readout, 0);

    ViewCell entry;
    entry.cell = cell;
    entry.readout = readout;
    entry.hoverToken = view->addHoverPickListener(
        [this, view](const View3D::ViewportPickResult& result) {
            updateCoordinateReadout(view, result);
        });
    m_cells.insert(view, entry);
    return cell;
}

void ViewportContainer::updateCoordinateReadout(View3D::IRenderSurface* view,
                                                const View3D::ViewportPickResult& result)
{
    const auto entry = m_cells.constFind(view);
    if (entry == m_cells.constEnd() || !entry->readout)
        return;
    if (!result.hasWorldPosition) {
        entry->readout->setText(QStringLiteral("坐标 —"));
        return;
    }
    const std::array<double, 3>& point = result.worldPosition;
    // 球面模式的渲染空间是「ECEF − 锚点」：加回锚点再换算经纬度，局部 XYZ
    // 在这时没有地理意义，读数只给经纬度与椭球高。
    if (m_scene && m_scene->globeModeEnabled()) {
        std::array<double, 3> anchor {};
        if (m_scene->globeRenderOrigin(&anchor)) {
            const std::array<double, 3> ecef = {
                point[0] + anchor[0], point[1] + anchor[1], point[2] + anchor[2]};
            double lonDeg = 0.0;
            double latDeg = 0.0;
            double heightMeters = 0.0;
            if (View3D::GlobeMath::ecefToGeodetic(ecef, &lonDeg, &latDeg, &heightMeters)) {
                QString text = QStringLiteral("坐标 经度 %1°  纬度 %2°  椭球高 %3 m")
                                   .arg(lonDeg, 0, 'f', 6)
                                   .arg(latDeg, 0, 'f', 6)
                                   .arg(heightMeters, 0, 'f', 1);
                // renderKey 为空说明这是视线与椭球面的交点，不是瓦片表面。
                if (result.renderKey.isEmpty())
                    text += QStringLiteral("（椭球面）");
                entry->readout->setText(text);
                return;
            }
        }
    }
    QString text = QStringLiteral("X %1  Y %2  Z %3 m")
                       .arg(point[0], 0, 'f', 2)
                       .arg(point[1], 0, 'f', 2)
                       .arg(point[2], 0, 'f', 2);
    // renderKey 为空说明这是视线与地面的交点，不是模型表面，读数要分清。
    if (result.renderKey.isEmpty())
        text += QStringLiteral("（地面）");
    auto* reference = m_pluginContainer
        ? m_pluginContainer->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double heightMeters = 0.0;
    if (reference && reference->ready() &&
        reference->localToLonLatHeight(point, &lonDeg, &latDeg, &heightMeters)) {
        text += QStringLiteral("｜ %1°, %2°, 椭球高 %3 m")
                    .arg(lonDeg, 0, 'f', 6)
                    .arg(latDeg, 0, 'f', 6)
                    .arg(heightMeters, 0, 'f', 1);
    }
    entry->readout->setText(text);
}

void ViewportContainer::rebuildLayout()
{
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        // 只移除布局项，不把原生渲染窗口设成顶层窗口；否则窗口句柄会被重建。
        delete item;
    }
    for (int row = 0; row < 2; ++row)
        m_grid->setRowStretch(row, 0);
    for (int column = 0; column < 2; ++column)
        m_grid->setColumnStretch(column, 0);
    if (m_layout == View3D::ViewportLayout::Single) {
        for (View3D::IRenderSurface* view : m_views)
            m_grid->addWidget(cellFor(view), 0, 0);
        m_grid->setRowStretch(0, 1);
        m_grid->setColumnStretch(0, 1);
    } else if (m_layout == View3D::ViewportLayout::Dual) {
        for (int i = 0; i < m_views.size() && i < 2; ++i)
            m_grid->addWidget(cellFor(m_views[i]), 0, i);
        m_grid->setRowStretch(0, 1);
        m_grid->setColumnStretch(0, 1);
        m_grid->setColumnStretch(1, 1);
    } else {
        for (int i = 0; i < m_views.size() && i < 4; ++i)
            m_grid->addWidget(cellFor(m_views[i]), i / 2, i % 2);
        for (int row = 0; row < 2; ++row)
            m_grid->setRowStretch(row, 1);
        for (int column = 0; column < 2; ++column)
            m_grid->setColumnStretch(column, 1);
    }
}

void ViewportContainer::applyVisibility()
{
    const int targetCount = layoutViewportCount();
    for (int index = 0; index < m_views.size(); ++index) {
        const bool visible = index < targetCount;
        m_views[index]->setRenderWindowVisible(visible);
        // 藏的是格子容器：只藏渲染窗口的话，底部读数条会留在网格里变成空行。
        const auto cell = m_cells.constFind(m_views[index]);
        if (cell != m_cells.constEnd() && cell->cell)
            cell->cell->setVisible(visible);
        if (QWidget* widget = m_views[index]->widget())
            widget->setVisible(visible);
    }
}

} // namespace View3DPlugins
