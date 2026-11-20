#include "OgreViewportSurface.h"

#include "OgreRuntime.h"
#include "OgreScene.h"

#include <GlobeMath.h>
#include <ModelMesh.h>
#include <mutex>

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreColourValue.h>
#include <OgreException.h>
#include <OgreGpuProgramParams.h>
#include <OgreHighLevelGpuProgram.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreHlms.h>
#include <OgreHlmsDatablock.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreIdString.h>
#include <OgreImage2.h>
#include <OgreLight.h>
#include <OgreManualObject2.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgreMaterialManager.h>
#include <OgreMatrix4.h>
#include <OgrePass.h>
#include <OgrePixelFormatGpu.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreQuaternion.h>
#include <OgreRay.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreStagingTexture.h>
#include <OgreStringConverter.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <OgreVector3.h>
#include <OgreVector4.h>
#include <OgreVisibilityFlags.h>
#include <OgreWindow.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QRectF>
#include <QResizeEvent>
#include <QSizeF>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>

namespace View3D {

OgreViewportSurface::OgreViewportSurface(OgreRuntime& runtime, OgreScene& scene,
                                         QWidget* parent)
    : QWidget(parent), m_runtime(runtime), m_scene(scene)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    static std::atomic<std::uint64_t> viewportSerial {0};
    m_instanceSuffix = std::to_string(++viewportSerial);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    // 悬停拾取要吃「没有按键按下」的 MouseMove：Qt 在没有开启 mouse tracking 时
    // 不会把这种移动投递给控件，底部坐标读数就永远不刷新（原来那条单槽悬停回调
    // 是死代码，也是这个原因）。
    setMouseTracking(true);
    setMinimumSize(320, 240);
    m_activityTimer.setInterval(100);
    connect(&m_activityTimer, &QTimer::timeout, this, &OgreViewportSurface::updateRenderActive);
}

OgreViewportSurface::~OgreViewportSurface()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    shutdown();
}

bool OgreViewportSurface::initialize(const QString& runtimeDirectory)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (ready())
        return true;
    m_root = m_runtime.root();
    if (!m_root)
        return false;
    try {
        const qreal scale = devicePixelRatioF();
        Ogre::NameValuePairList parameters;
        parameters["externalWindowHandle"] =
            Ogre::StringConverter::toString(static_cast<std::uintptr_t>(winId()));
        parameters["gamma"] = "Yes";
        parameters["vsync"] = OgreRuntime::vsyncEnabled() ? "Yes" : "No";
        parameters["reverse_depth"] = "Yes";
        m_window = m_root->createRenderWindow(
            "View3DViewport" + m_instanceSuffix,
            static_cast<Ogre::uint32>(qMax(1, qRound(width() * scale))),
            static_cast<Ogre::uint32>(qMax(1, qRound(height() * scale))), false,
            &parameters);
        m_window->setHidden(!m_renderWindowVisible.load(std::memory_order_relaxed));
        if (!m_scene.ensureReady(runtimeDirectory)) {
            shutdown();
            return false;
        }
        m_sceneManager = m_scene.sceneManager();
        m_camera = m_sceneManager->createCamera("View3DCamera" + m_instanceSuffix);
        m_camera->setPosition(Ogre::Vector3(0.0f, 2.0f, 8.0f));
        m_camera->lookAt(Ogre::Vector3::ZERO);
        m_cameraTarget = {0.0, 0.0, 0.0};
        m_camera->setNearClipDistance(0.1f);
        m_camera->setFarClipDistance(100000.0f);
        m_camera->setAutoAspectRatio(true);
        m_visibilityBit = m_scene.acquireViewSlot(m_camera);
        if (!createBackgroundResources() || !createViewportWorkspace() ||
            !updateBackgroundConstants()) {
            shutdown();
            return false;
        }
        m_navigationElapsed.start();
        m_readyFlag.store(true, std::memory_order_relaxed);
        updateRenderActive();
        m_activityTimer.start();
        std::cerr << "VIEW3D_OGRE_STAGE: surface_ready viewport=" << m_instanceSuffix
                  << " viewbit=0x" << std::hex << m_visibilityBit << std::dec << "\n";
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_OGRE_SURFACE_FAILED: " << error.getFullDescription() << "\n";
        shutdown();
        return false;
    }
}

void OgreViewportSurface::shutdown()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_activityTimer.stop();
    m_readyFlag.store(false, std::memory_order_relaxed);
    m_renderActive.store(false, std::memory_order_relaxed);
    if (m_root && m_workspace)
        m_root->getCompositorManager2()->removeWorkspace(m_workspace);
    m_workspace = nullptr;
    clearMeasurementOverlayResources();
    clearClippingHelperResources();
    destroyViewportWorkspaceDefinitions();
    destroyShadowNodeDefinitions();
    destroyBackgroundResources();
    if (m_root && m_window && m_root->getRenderSystem())
        m_root->getRenderSystem()->destroyRenderWindow(m_window);
    m_window = nullptr;
    m_camera = nullptr;
    m_sceneManager = nullptr;
    m_root = nullptr;
    if (m_visibilityBit != 0u) {
        m_scene.releaseViewSlot(m_visibilityBit);
        m_visibilityBit = 0u;
    }
}

std::uint32_t OgreViewportSurface::viewVisibilityMask() const noexcept
{
    return m_visibilityBit != 0u ? m_visibilityBit : Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
}

void OgreViewportSurface::prepareFrame()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!renderActive())
        return;
    const double elapsedSeconds = m_navigationElapsed.isValid()
        ? qBound(0.0, m_navigationElapsed.restart() / 1000.0, 0.1)
        : 0.0;
    const double rightAxis = (m_navigationKeys.contains(Qt::Key_D) ? 1.0 : 0.0) -
        (m_navigationKeys.contains(Qt::Key_A) ? 1.0 : 0.0);
    const double upAxis = (m_navigationKeys.contains(Qt::Key_E) ? 1.0 : 0.0) -
        (m_navigationKeys.contains(Qt::Key_Q) ? 1.0 : 0.0);
    const double forwardAxis = (m_navigationKeys.contains(Qt::Key_W) ? 1.0 : 0.0) -
        (m_navigationKeys.contains(Qt::Key_S) ? 1.0 : 0.0);
    if (rightAxis != 0.0 || upAxis != 0.0 || forwardAxis != 0.0) {
        moveCameraContinuously(rightAxis, upAxis, forwardAxis, elapsedSeconds,
                               m_navigationKeys.contains(Qt::Key_Shift));
    }
    // 球面锚点跟随本视口相机；多视口时最后一个 prepareFrame 的视口定锚，
    // renderCycle 随后统一调 syncGlobeCamera 把所有相机摆回最终锚点。
    if (m_globeMode && m_globeCameraValid)
        m_scene.setGlobeRenderOrigin(m_globePositionEcef);
    updateMeasurementOverlayForCamera();
}

void OgreViewportSurface::updateRenderActive()
{
    QWidget* topLevel = window();
    const bool active = m_readyFlag.load(std::memory_order_relaxed) &&
        m_renderWindowVisible.load(std::memory_order_relaxed) && isVisible() &&
        topLevel && !topLevel->isMinimized();
    m_renderActive.store(active, std::memory_order_relaxed);
    // 停用的 workspace 不会被 renderOneFrame 遍历，隐藏视口因此完全不产生绘制开销。
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_workspace)
        m_workspace->setEnabled(active);
}

bool OgreViewportSurface::ready() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_root && m_window && m_sceneManager && m_camera && m_workspace &&
        m_scene.ready();
}

IRenderScene* OgreViewportSurface::scene() const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return const_cast<OgreScene*>(&m_scene);
}

void OgreViewportSurface::setRenderWindowVisible(bool visible)
{
    m_renderWindowVisible.store(visible, std::memory_order_relaxed);
    updateRenderActive();
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_window)
        m_window->setHidden(!visible);
}


bool OgreViewportSurface::setDisplayMode(ViewportDisplayMode mode)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setDisplayMode(mode);
}

ViewportDisplayMode OgreViewportSurface::displayMode() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.displayMode();
}

bool OgreViewportSurface::setGridVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setGridVisible(visible);
}

bool OgreViewportSurface::gridVisible() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.gridVisible();
}

bool OgreViewportSurface::setAxesVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setAxesVisible(visible);
}

bool OgreViewportSurface::axesVisible() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.axesVisible();
}

bool OgreViewportSurface::setEnvironmentStyle(const ViewportEnvironmentStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setEnvironmentStyle(style);
}

ViewportEnvironmentStyle OgreViewportSurface::environmentStyle() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.environmentStyle();
}

bool OgreViewportSurface::setLightingStyle(const ViewportLightingStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setLightingStyle(style);
}

ViewportLightingStyle OgreViewportSurface::lightingStyle() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.lightingStyle();
}

bool OgreViewportSurface::setAmbientLightVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setAmbientLightVisible(visible);
}

bool OgreViewportSurface::setMainLightVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_scene.setMainLightVisible(visible);
}

bool OgreViewportSurface::focusBounds(const RenderBounds& bounds)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || !bounds.valid)
        return false;
    if (m_planarMode)
        return focusBoundsPlanar(bounds);
    if (m_globeMode)
        return globeFocusBounds(bounds);
    const Ogre::Vector3 minimum(
        static_cast<Ogre::Real>(bounds.minimum[0]),
        static_cast<Ogre::Real>(bounds.minimum[1]),
        static_cast<Ogre::Real>(bounds.minimum[2]));
    const Ogre::Vector3 maximum(
        static_cast<Ogre::Real>(bounds.maximum[0]),
        static_cast<Ogre::Real>(bounds.maximum[1]),
        static_cast<Ogre::Real>(bounds.maximum[2]));
    const Ogre::Vector3 center = (minimum + maximum) * 0.5f;
    const Ogre::Real radius = qMax<Ogre::Real>((maximum - minimum).length() * 0.5f, 0.1f);
    m_camera->setPosition(center + Ogre::Vector3(0.0f, radius * 0.35f, radius * 2.5f));
    m_camera->lookAt(center);
    m_cameraTarget = {center.x, center.y, center.z};
    m_sceneRadius = qMax(m_sceneRadius, static_cast<double>(radius));
    updateClipDistances((m_camera->getDerivedPosition() - center).length());
    return true;
}

bool OgreViewportSurface::focusBoundsPlanar(const RenderBounds& bounds)
{
    if (!m_camera || !bounds.valid)
        return false;
    const Ogre::Real spanX = static_cast<Ogre::Real>(bounds.maximum[0] - bounds.minimum[0]);
    const Ogre::Real spanZ = static_cast<Ogre::Real>(bounds.maximum[2] - bounds.minimum[2]);
    // 二维只看水平投影：高程方向的尺寸不参与构图。
    const Ogre::Real span = qMax<Ogre::Real>(qMax<Ogre::Real>(spanX, spanZ), 0.1f);
    const Ogre::Vector3 center(
        static_cast<Ogre::Real>((bounds.minimum[0] + bounds.maximum[0]) * 0.5),
        static_cast<Ogre::Real>((bounds.minimum[1] + bounds.maximum[1]) * 0.5),
        static_cast<Ogre::Real>((bounds.minimum[2] + bounds.maximum[2]) * 0.5));
    m_cameraTarget = {center.x, center.y, center.z};
    m_sceneRadius = qMax(m_sceneRadius, static_cast<double>(span * 0.5f));
    m_camera->setProjectionType(Ogre::ProjectionType::PT_ORTHOGRAPHIC);
    m_camera->setOrthoWindowHeight(span * 1.2f);
    const Ogre::Real distance = qMax<Ogre::Real>(span, 1.0f);
    m_camera->setFixedYawAxis(true, Ogre::Vector3::NEGATIVE_UNIT_Z);
    m_camera->setPosition(center + Ogre::Vector3(0.0f, distance, 0.0f));
    m_camera->setDirection(Ogre::Vector3::NEGATIVE_UNIT_Y);
    updateClipDistances(static_cast<double>(distance));
    return true;
}

// 近裁剪面跟随观察枢轴，而不是数据集半径：按半径定出的 1.5 m 近裁剪面
// 会把贴近观察的表面整块切掉，看起来像"再也放不大"。
void OgreViewportSurface::updateClipDistances(double distanceToTarget)
{
    if (!m_camera) return;
    const double nearDistance = std::clamp(distanceToTarget * 0.001, 0.02, 1.0);
    const double farDistance = std::max({100.0, m_sceneRadius * 20.0,
                                         distanceToTarget * 4.0});
    m_camera->setNearClipDistance(static_cast<Ogre::Real>(nearDistance));
    m_camera->setFarClipDistance(static_cast<Ogre::Real>(farDistance));
}

bool OgreViewportSurface::focusModelLayer(const QString& layerKey)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    RenderBounds bounds;
    return m_scene.modelLayerBounds(layerKey, &bounds) && focusBounds(bounds);
}

bool OgreViewportSurface::focusModelLayerNode(const QString& layerKey,
                                              std::uint32_t nodeIndex)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    RenderBounds bounds;
    return m_scene.modelLayerNodeBounds(layerKey, nodeIndex, &bounds) &&
        focusBounds(bounds);
}

bool OgreViewportSurface::focusTileGroup(const QString& tileGroupKey)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    RenderBounds bounds;
    return m_scene.tileGroupBounds(tileGroupKey, &bounds) && focusBounds(bounds);
}

bool OgreViewportSurface::focusAllVisible()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    RenderBounds bounds;
    return m_scene.visibleBounds(&bounds) && focusBounds(bounds);
}

bool OgreViewportSurface::setOverlay(
    const std::vector<std::array<double, 3>>& points, bool connectPoints)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return setMeasurementOverlay(points, connectPoints);
}

void OgreViewportSurface::clearOverlay()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearMeasurementOverlay();
}

bool OgreViewportSurface::hasOverlay() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return !m_measurementOverlayPoints.empty();
}

PickListenerId OgreViewportSurface::addPickListener(
    std::function<void(const ViewportPickResult&)> listener)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!listener)
        return 0;
    const PickListenerId id = m_nextPickListenerId++;
    m_pickListeners.push_back({id, std::move(listener)});
    return id;
}

bool OgreViewportSurface::removePickListener(PickListenerId id)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (id == 0)
        return false;
    for (auto it = m_pickListeners.begin(); it != m_pickListeners.end(); ++it) {
        if (it->id == id) {
            m_pickListeners.erase(it);
            return true;
        }
    }
    return false;
}

void OgreViewportSurface::dispatchPick(const ViewportPickResult& result)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const std::vector<PickListener> snapshot = m_pickListeners;
    for (const PickListener& listener : snapshot) {
        if (listener.callback)
            listener.callback(result);
    }
}

PickListenerId OgreViewportSurface::addHoverPickListener(
    std::function<void(const ViewportPickResult&)> listener)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!listener)
        return 0;
    const PickListenerId id = m_nextPickListenerId++;
    m_hoverListeners.push_back({id, std::move(listener)});
    return id;
}

bool OgreViewportSurface::removeHoverPickListener(PickListenerId id)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (id == 0)
        return false;
    for (auto it = m_hoverListeners.begin(); it != m_hoverListeners.end(); ++it) {
        if (it->id == id) {
            m_hoverListeners.erase(it);
            return true;
        }
    }
    return false;
}

void OgreViewportSurface::dispatchHover(const ViewportPickResult& result)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const std::vector<PickListener> snapshot = m_hoverListeners;
    for (const PickListener& listener : snapshot) {
        if (listener.callback)
            listener.callback(result);
    }
}

void OgreViewportSurface::setRectSelectCallback(
    std::function<void(const QList<QString>&, bool additive)> callback)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_rectSelectCallback = std::move(callback);
}

QImage OgreViewportSurface::captureImage(const QSize& outputSize,
                                         const CaptureOptions& options)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!ready() || !m_window->getTexture())
        return {};

    struct CaptureGuard {
        OgreViewportSurface& self;
        bool gridVisible;
        bool axesVisible;
        bool overlayVisible;
        bool originalManualSwap;
        bool originalWorkspaceEnabled;
        ~CaptureGuard()
        {
            self.m_scene.setGridVisible(gridVisible);
            self.m_scene.setAxesVisible(axesVisible);
            if (self.m_measurementOverlayObject)
                self.m_measurementOverlayObject->setVisible(overlayVisible);
            if (self.m_workspace)
                self.m_workspace->setEnabled(originalWorkspaceEnabled);
            if (self.m_window) {
                if (!originalManualSwap && self.m_window->isManualSwapRelease())
                    self.m_window->performManualRelease();
                self.m_window->setManualSwapRelease(originalManualSwap);
                self.m_window->setWantsToDownload(false);
            }
        }
    } guard { *this, m_scene.gridVisible(), m_scene.axesVisible(),
              m_measurementOverlayObject && m_measurementOverlayObject->isVisible(),
              m_window->isManualSwapRelease(),
              m_workspace && m_workspace->getEnabled() };

    try {
        m_scene.setGridVisible(options.includeGrid);
        m_scene.setAxesVisible(options.includeAxes);
        if (m_measurementOverlayObject)
            m_measurementOverlayObject->setVisible(options.includeMeasurementOverlay);
        m_window->setWantsToDownload(true);
        m_window->setManualSwapRelease(true);
        if (m_workspace)
            m_workspace->setEnabled(true);
        m_root->renderOneFrame();
        if (!m_window->canDownloadData())
            return {};
        Ogre::TextureGpu* texture = m_window->getTexture();
        Ogre::Image2 downloaded;
        downloaded.convertFromTexture(texture, 0u, 0u);
        const Ogre::TextureBox pixels = downloaded.getData(0u);
        const Ogre::PixelFormatGpu pixelFormat = downloaded.getPixelFormat();
        QImage image(static_cast<int>(downloaded.getWidth()),
                     static_cast<int>(downloaded.getHeight()), QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            auto* destination = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const Ogre::ColourValue colour = pixels.getColourAt(
                    static_cast<std::size_t>(x), static_cast<std::size_t>(y), 0u,
                    pixelFormat);
                destination[x] = qRgba(qBound(0, qRound(colour.r * 255.0f), 255),
                                       qBound(0, qRound(colour.g * 255.0f), 255),
                                       qBound(0, qRound(colour.b * 255.0f), 255),
                                       qBound(0, qRound(colour.a * 255.0f), 255));
            }
        }
        if (outputSize.isValid() && outputSize != image.size())
            image = image.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return image;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_CAPTURE_FAILED: " << error.getFullDescription() << "\n";
        return {};
    }
}

QImage OgreViewportSurface::captureImageOffscreen(const QSize& outputSize,
                                                  const CaptureOptions& options)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!ready() || outputSize.width() <= 0 || outputSize.height() <= 0)
        return {};

    struct CaptureGuard {
        OgreViewportSurface& self;
        bool gridVisible;
        bool axesVisible;
        bool overlayVisible;
        bool manualSwap;
        ~CaptureGuard()
        {
            self.m_scene.setGridVisible(gridVisible);
            self.m_scene.setAxesVisible(axesVisible);
            if (self.m_measurementOverlayObject)
                self.m_measurementOverlayObject->setVisible(overlayVisible);
            if (self.m_window)
                self.m_window->setManualSwapRelease(manualSwap);
        }
    } guard { *this, m_scene.gridVisible(), m_scene.axesVisible(),
              m_measurementOverlayObject && m_measurementOverlayObject->isVisible(),
              m_window->isManualSwapRelease() };

    Ogre::TextureGpu* offscreen = nullptr;
    Ogre::CompositorWorkspace* workspace = nullptr;
    try {
        m_scene.setGridVisible(options.includeGrid);
        m_scene.setAxesVisible(options.includeAxes);
        if (m_measurementOverlayObject)
            m_measurementOverlayObject->setVisible(options.includeMeasurementOverlay);

        Ogre::TextureGpuManager* textureManager =
            m_root->getRenderSystem()->getTextureGpuManager();
        offscreen = textureManager->createTexture(
            "View3DOffscreenCapture" +
                Ogre::StringConverter::toString(nextOgreResourceSerial()),
            Ogre::GpuPageOutStrategy::Discard,
            Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::ManualTexture,
            Ogre::TextureTypes::Type2D);
        offscreen->setResolution(static_cast<Ogre::uint32>(outputSize.width()),
                                 static_cast<Ogre::uint32>(outputSize.height()));
        offscreen->setPixelFormat(Ogre::PFG_RGBA8_UNORM_SRGB);
        offscreen->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        offscreen->waitForData();

        Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
        const std::string& workspaceName = m_shadowRuntime.enabled
            ? m_shadowWorkspaceNames[shadowQualityIndex()]
            : m_backgroundWorkspaceName;
        workspace = compositor->addWorkspace(m_sceneManager, offscreen, m_camera,
                                             Ogre::IdString(workspaceName), false);
        if (!workspace)
            throw Ogre::Exception(Ogre::Exception::ERR_INVALIDPARAMS,
                                  "failed to create offscreen workspace",
                                  "captureImageOffscreen");
        m_window->setManualSwapRelease(false);
        workspace->setEnabled(true);
        updateMeasurementOverlayForCamera();
        m_root->renderOneFrame();

        Ogre::Image2 downloaded;
        downloaded.convertFromTexture(offscreen, 0u, 0u, true);
        const Ogre::TextureBox pixels = downloaded.getData(0u);
        QImage image(static_cast<int>(downloaded.getWidth()),
                     static_cast<int>(downloaded.getHeight()), QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            auto* destination = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const Ogre::ColourValue colour = pixels.getColourAt(
                    static_cast<std::size_t>(x), static_cast<std::size_t>(y), 0u,
                    downloaded.getPixelFormat());
                destination[x] = qRgba(qBound(0, qRound(colour.r * 255.0f), 255),
                                       qBound(0, qRound(colour.g * 255.0f), 255),
                                       qBound(0, qRound(colour.b * 255.0f), 255),
                                       qBound(0, qRound(colour.a * 255.0f), 255));
            }
        }
        compositor->removeWorkspace(workspace);
        workspace = nullptr;
        textureManager->destroyTexture(offscreen);
        offscreen = nullptr;
        return image;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_OFFSCREEN_CAPTURE_FAILED: "
                  << error.getFullDescription() << "\n";
        if (m_root) {
            if (workspace) {
                m_root->getCompositorManager2()->removeWorkspace(workspace);
                workspace = nullptr;
            }
            if (offscreen) {
                m_root->getRenderSystem()->getTextureGpuManager()->destroyTexture(offscreen);
                offscreen = nullptr;
            }
        }
        return {};
    }
}

void OgreViewportSurface::clearMeasurementOverlayResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && m_measurementOverlayObject)
        m_sceneManager->destroyManualObject(m_measurementOverlayObject);
    m_measurementOverlayObject = nullptr;
    if (m_root && m_measurementOverlayDatablock &&
        !m_measurementOverlayDatablockName.empty()) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit)
            unlit->destroyDatablock(Ogre::IdString(m_measurementOverlayDatablockName));
    }
    m_measurementOverlayDatablock = nullptr;
    m_measurementOverlayDatablockName.clear();
    m_measurementOverlayCamera = {};
}

bool OgreViewportSurface::rebuildMeasurementOverlay()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearMeasurementOverlayResources();
    if (m_measurementOverlayPoints.empty() || !m_sceneManager || !m_root)
        return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit)
            return false;
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = false;
        // 测量点/连线与模型表面共面；reverse-Z 下正偏置才是“靠近相机”。
        // 实测不加偏置或负偏置都会被模型深度挡住，辅助线完全不可见。
        macroblock.mDepthBiasConstant = 1.0f;
        macroblock.mDepthBiasSlopeScale = 1.0f;
        m_measurementOverlayDatablockName = "View3DMeasurementOverlay" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_measurementOverlayDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_measurementOverlayDatablockName,
                                   m_measurementOverlayDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        m_measurementOverlayDatablock->setUseColour(true);
        m_measurementOverlayDatablock->setColour(
            Ogre::ColourValue(1.0f, 0.78f, 0.08f, 1.0f));
        m_measurementOverlayObject = m_sceneManager->createManualObject();
        m_measurementOverlayObject->setCastShadows(false);
        m_measurementOverlayObject->setVisibilityFlags(viewVisibilityMask());
        m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
            ->attachObject(m_measurementOverlayObject);
        rebuildMeasurementOverlayGeometry();
        return true;
    } catch (const Ogre::Exception&) {
        clearMeasurementOverlayResources();
        return false;
    }
}

void OgreViewportSurface::rebuildMeasurementOverlayGeometry()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_measurementOverlayObject || !m_camera || !m_window ||
        m_measurementOverlayPoints.empty())
        return;
    // 辅助点/线按固定像素绘制：每次相机变化都按当前视距把像素尺寸折算成世界尺寸。
    constexpr Ogre::Real kPointRadiusPixels = 4.0f;
    constexpr Ogre::Real kLineHalfWidthPixels = 1.25f;
    const Ogre::Real renderHeight =
        static_cast<Ogre::Real>(qMax(1u, m_window->getHeight()));
    const bool orthographic =
        m_camera->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Real tanHalfFov =
        std::tan(m_camera->getFOVy().valueRadians() * 0.5f);
    const Ogre::Real orthoHeight = m_camera->getOrthoWindowHeight();
    const Ogre::Vector3 cameraPosition = m_camera->getDerivedPosition();
    const Ogre::Vector3 cameraDirection = m_camera->getDerivedDirection();
    const Ogre::Vector3 cameraRight = m_camera->getDerivedRight();
    const Ogre::Vector3 cameraUp = m_camera->getDerivedUp();
    const Ogre::Matrix4 viewMatrix = m_camera->getViewMatrix();
    const auto worldPerPixel = [&](const Ogre::Vector3& point) {
        if (orthographic)
            return orthoHeight / renderHeight;
        const Ogre::Real depth =
            qMax((point - cameraPosition).dotProduct(cameraDirection), 1e-4f);
        return 2.0f * depth * tanHalfFov / renderHeight;
    };
    const auto screenDirection = [&](const Ogre::Vector3& first,
                                     const Ogre::Vector3& second,
                                     std::array<Ogre::Real, 2>* direction) {
        const Ogre::Vector3 firstCamera = viewMatrix.transformAffine(first);
        const Ogre::Vector3 secondCamera = viewMatrix.transformAffine(second);
        const Ogre::Real firstDepth = qMax(-firstCamera.z, 1e-4f);
        const Ogre::Real secondDepth = qMax(-secondCamera.z, 1e-4f);
        const Ogre::Real dx =
            secondCamera.x / secondDepth - firstCamera.x / firstDepth;
        const Ogre::Real dy =
            secondCamera.y / secondDepth - firstCamera.y / firstDepth;
        const Ogre::Real length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-6f) {
            *direction = {1.0f, 0.0f};
            return;
        }
        *direction = {dx / length, dy / length};
    };
    // 球面：测量点存的是 ECEF，叠加层按当前锚点换到渲染空间（ECEF−锚点）。
    std::array<double, 3> overlayAnchor { 0.0, 0.0, 0.0 };
    const bool overlayGlobe = m_globeMode && m_scene.globeRenderOrigin(&overlayAnchor);
    const auto toVector = [&](const std::array<double, 3>& point) {
        return Ogre::Vector3(static_cast<Ogre::Real>(
                                 point[0] - (overlayGlobe ? overlayAnchor[0] : 0.0)),
                             static_cast<Ogre::Real>(
                                 point[1] - (overlayGlobe ? overlayAnchor[1] : 0.0)),
                             static_cast<Ogre::Real>(
                                 point[2] - (overlayGlobe ? overlayAnchor[2] : 0.0)));
    };

    m_measurementOverlayObject->clear();
    m_measurementOverlayObject->begin(m_measurementOverlayDatablockName,
                                      Ogre::OT_TRIANGLE_LIST);
    std::uint32_t vertex = 0;
    const auto appendVertex = [&](const Ogre::Vector3& point) {
        m_measurementOverlayObject->position(point);
        m_measurementOverlayObject->colour(1.0f, 0.78f, 0.08f, 1.0f);
        m_measurementOverlayObject->index(vertex++);
    };
    const auto appendTriangle = [&](const Ogre::Vector3& a, const Ogre::Vector3& b,
                                    const Ogre::Vector3& c) {
        appendVertex(a);
        appendVertex(b);
        appendVertex(c);
    };
    const auto appendQuad = [&](const Ogre::Vector3& a, const Ogre::Vector3& b,
                                const Ogre::Vector3& c, const Ogre::Vector3& d) {
        appendTriangle(a, b, c);
        appendTriangle(c, b, d);
    };

    for (const std::array<double, 3>& pointArray : m_measurementOverlayPoints) {
        const Ogre::Vector3 point = toVector(pointArray);
        const Ogre::Real radius = kPointRadiusPixels * worldPerPixel(point);
        const Ogre::Vector3 right = cameraRight * radius;
        const Ogre::Vector3 up = cameraUp * radius;
        // 面向相机的菱形，半径按当前视距折算成固定像素。
        appendTriangle(point + up, point - right, point + right);
        appendTriangle(point - up, point + right, point - right);
    }
    if (m_measurementOverlayConnectPoints && m_measurementOverlayPoints.size() > 1) {
        for (std::size_t index = 1; index < m_measurementOverlayPoints.size(); ++index) {
            const Ogre::Vector3 first = toVector(m_measurementOverlayPoints[index - 1]);
            const Ogre::Vector3 second = toVector(m_measurementOverlayPoints[index]);
            std::array<Ogre::Real, 2> direction {1.0f, 0.0f};
            screenDirection(first, second, &direction);
            const std::array<Ogre::Real, 2> perpendicular {-direction[1], direction[0]};
            const Ogre::Real firstWidth = kLineHalfWidthPixels * worldPerPixel(first);
            const Ogre::Real secondWidth = kLineHalfWidthPixels * worldPerPixel(second);
            const Ogre::Vector3 firstOffset =
                (cameraRight * perpendicular[0] + cameraUp * perpendicular[1]) *
                firstWidth;
            const Ogre::Vector3 secondOffset =
                (cameraRight * perpendicular[0] + cameraUp * perpendicular[1]) *
                secondWidth;
            appendQuad(first + firstOffset, first - firstOffset, second + secondOffset,
                       second - secondOffset);
        }
    }
    m_measurementOverlayObject->end()->setDatablock(m_measurementOverlayDatablock);
    m_measurementOverlayCamera = measurementOverlayCameraState();
}

void OgreViewportSurface::updateMeasurementOverlayForCamera()
{
    if (!m_measurementOverlayObject || m_measurementOverlayPoints.empty())
        return;
    if (measurementOverlayCameraState() == m_measurementOverlayCamera)
        return;
    rebuildMeasurementOverlayGeometry();
}

OgreViewportSurface::MeasurementOverlayCameraState
OgreViewportSurface::measurementOverlayCameraState() const
{
    MeasurementOverlayCameraState state;
    if (!m_camera || !m_window)
        return state;
    const Ogre::Vector3 position = m_camera->getDerivedPosition();
    const Ogre::Vector3 direction = m_camera->getDerivedDirection();
    const Ogre::Vector3 up = m_camera->getDerivedUp();
    state.position = {position.x, position.y, position.z};
    state.direction = {direction.x, direction.y, direction.z};
    state.up = {up.x, up.y, up.z};
    state.verticalFov = m_camera->getFOVy().valueRadians();
    state.orthoHeight = m_camera->getOrthoWindowHeight();
    state.projectionType = static_cast<int>(m_camera->getProjectionType());
    state.renderWidth = static_cast<int>(m_window->getWidth());
    state.renderHeight = static_cast<int>(m_window->getHeight());
    state.globe = m_globeMode && m_scene.globeRenderOrigin(&state.globeAnchor);
    return state;
}

bool OgreViewportSurface::setMeasurementOverlay(
    const std::vector<std::array<double, 3>>& points, bool connectPoints)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    for (const auto& point : points) {
        if (!qIsFinite(point[0]) || !qIsFinite(point[1]) || !qIsFinite(point[2]))
            return false;
    }
    m_measurementOverlayPoints = points;
    m_measurementOverlayConnectPoints = connectPoints;
    return rebuildMeasurementOverlay();
}

void OgreViewportSurface::clearMeasurementOverlay()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_measurementOverlayPoints.clear();
    m_measurementOverlayConnectPoints = false;
    clearMeasurementOverlayResources();
}


bool OgreViewportSurface::setBackgroundStyle(const ViewportBackgroundStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto validComponent = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (!validComponent(style.topRed) || !validComponent(style.topGreen) ||
        !validComponent(style.topBlue) || !validComponent(style.bottomRed) ||
        !validComponent(style.bottomGreen) || !validComponent(style.bottomBlue))
        return false;
    const ViewportBackgroundStyle previous = m_backgroundStyle;
    m_backgroundStyle = style;
    if (!m_root || updateBackgroundConstants()) return true;
    m_backgroundStyle = previous;
    updateBackgroundConstants();
    return false;
}

bool OgreViewportSurface::setBackgroundMode(ViewportBackgroundMode mode)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const ViewportBackgroundMode previous = m_backgroundStyle.mode;
    m_backgroundStyle.mode = mode;
    if (!m_root || updateBackgroundConstants()) return true;
    m_backgroundStyle.mode = previous;
    updateBackgroundConstants();
    return false;
}

bool OgreViewportSurface::createBackgroundResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root) return false;
    try {
        m_backgroundVertexProgramName = "View3DBackgroundVs" + m_instanceSuffix;
        m_backgroundFragmentProgramName = "View3DBackgroundPs" + m_instanceSuffix;
        m_backgroundMaterialName = "View3DBackgroundMaterial" + m_instanceSuffix;
        m_backgroundNodeName = "View3DBackgroundNode" + m_instanceSuffix;
        m_backgroundWorkspaceName = "View3DBackgroundWorkspace" + m_instanceSuffix;

        const Ogre::String vertexSource = R"hlsl(
struct VS_INPUT
{
    float4 vertex : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
};

struct PS_INPUT
{
    float2 uv0 : TEXCOORD0;
    float4 position : SV_Position;
};

PS_INPUT main(VS_INPUT input, uniform float4x4 worldViewProj)
{
    PS_INPUT output;
    output.position = mul(worldViewProj, input.vertex);
    output.uv0 = input.uv0;
    return output;
}
)hlsl";
        const Ogre::String fragmentSource = R"hlsl(
struct PS_INPUT
{
    float2 uv0 : TEXCOORD0;
    float4 position : SV_Position;
};

float4 main(PS_INPUT input,
            uniform float4 topColour,
            uniform float4 bottomColour) : SV_Target
{
    return lerp(topColour, bottomColour, saturate(input.uv0.y));
}
)hlsl";

        Ogre::HighLevelGpuProgramManager& programManager =
            Ogre::HighLevelGpuProgramManager::getSingleton();
        Ogre::HighLevelGpuProgramPtr vertexProgram = programManager.createProgram(
            m_backgroundVertexProgramName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, "hlsl",
            Ogre::GPT_VERTEX_PROGRAM);
        vertexProgram->setSource(vertexSource);
        vertexProgram->setParameter("entry_point", "main");
        vertexProgram->setParameter("target", "vs_5_0");
        vertexProgram->load();

        Ogre::HighLevelGpuProgramPtr fragmentProgram = programManager.createProgram(
            m_backgroundFragmentProgramName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, "hlsl",
            Ogre::GPT_FRAGMENT_PROGRAM);
        fragmentProgram->setSource(fragmentSource);
        fragmentProgram->setParameter("entry_point", "main");
        fragmentProgram->setParameter("target", "ps_5_0");
        fragmentProgram->load();

        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(
            m_backgroundMaterialName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        material->removeAllTechniques();
        Ogre::Pass* pass = material->createTechnique()->createPass();
        Ogre::HlmsMacroblock macroblock;
        macroblock.mDepthCheck = false;
        macroblock.mDepthWrite = false;
        macroblock.mCullMode = Ogre::CULL_NONE;
        pass->setMacroblock(macroblock);
        pass->setVertexProgram(m_backgroundVertexProgramName);
        pass->setFragmentProgram(m_backgroundFragmentProgramName);
        material->load();
        pass->getVertexProgramParameters()->setNamedAutoConstant(
            "worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_BACKGROUND_RESOURCE_FAILED: "
                  << error.getFullDescription() << "\n";
        return false;
    }
}

bool OgreViewportSurface::updateBackgroundConstants()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root || m_backgroundMaterialName.empty()) return !m_root;
    try {
        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(
            m_backgroundMaterialName);
        if (!material || material->getNumTechniques() == 0 ||
            material->getTechnique(0)->getNumPasses() == 0)
            return false;
        Ogre::GpuProgramParametersSharedPtr parameters =
            material->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
        const Ogre::Vector4 bottom(m_backgroundStyle.bottomRed,
                                   m_backgroundStyle.bottomGreen,
                                   m_backgroundStyle.bottomBlue, 1.0f);
        const Ogre::Vector4 top = m_backgroundStyle.mode == ViewportBackgroundMode::Solid
            ? bottom
            : Ogre::Vector4(m_backgroundStyle.topRed, m_backgroundStyle.topGreen,
                            m_backgroundStyle.topBlue, 1.0f);
        parameters->setNamedConstant("topColour", top);
        parameters->setNamedConstant("bottomColour", bottom);
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_BACKGROUND_CONSTANT_FAILED: "
                  << error.getFullDescription() << "\n";
        return false;
    }
}

bool OgreViewportSurface::createViewportWorkspace()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root || !m_sceneManager || !m_camera || !m_window ||
        m_backgroundMaterialName.empty())
        return false;
    return createShadowNodeDefinitions() && createViewportWorkspaceDefinitions() &&
        switchViewportWorkspace(m_shadowStyle.enabled, m_shadowStyle.quality);
}

std::size_t OgreViewportSurface::shadowQualityIndex() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_shadowRuntime.requestedQuality == ViewportShadowQuality::Performance)
        return 0u;
    if (m_shadowRuntime.requestedQuality == ViewportShadowQuality::Quality)
        return 2u;
    return 1u;
}

OgreViewportSurface::ShadowPreset OgreViewportSurface::shadowPreset(
    ViewportShadowQuality quality) noexcept
{
    switch (quality) {
    case ViewportShadowQuality::Performance:
        return {quality, ViewportShadowFilter::Pcf2x2, 2u,
                {512u, 512u, 0u, 0u}};
    case ViewportShadowQuality::Quality:
        return {quality, ViewportShadowFilter::Pcf4x4, 4u,
                {2048u, 2048u, 2048u, 2048u}};
    case ViewportShadowQuality::Balanced:
    default:
        return {ViewportShadowQuality::Balanced, ViewportShadowFilter::Pcf3x3, 3u,
                {1024u, 1024u, 1024u, 0u}};
    }
}

bool OgreViewportSurface::createShadowNodeDefinitions()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root) return false;
    m_shadowNodeNames = {"View3DShadowPerformance" + m_instanceSuffix,
                         "View3DShadowBalanced" + m_instanceSuffix,
                         "View3DShadowQuality" + m_instanceSuffix};
    try {
        Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
        const Ogre::RenderSystemCapabilities* capabilities =
            m_root->getRenderSystem()->getCapabilities();
        const std::array<ViewportShadowQuality, 3> qualities {
            ViewportShadowQuality::Performance, ViewportShadowQuality::Balanced,
            ViewportShadowQuality::Quality};
        for (std::size_t index = 0; index < qualities.size(); ++index) {
            if (compositor->hasShadowNodeDefinition(
                    Ogre::IdString(m_shadowNodeNames[index])))
                continue;
            const ShadowPreset preset = shadowPreset(qualities[index]);
            Ogre::ShadowNodeHelper::ShadowParam parameter;
            std::memset(&parameter, 0, sizeof(parameter));
            parameter.addLightType(Ogre::Light::LT_DIRECTIONAL);
            parameter.technique = Ogre::SHADOWMAP_PSSM;
            parameter.numPssmSplits = preset.splitCount;
            parameter.atlasId = 0u;
            const std::uint32_t cellSize = preset.resolution[0];
            for (std::uint8_t split = 0; split < preset.splitCount; ++split) {
                parameter.atlasStart[split].x =
                    static_cast<std::uint32_t>(split % 2u) * cellSize;
                parameter.atlasStart[split].y =
                    static_cast<std::uint32_t>(split / 2u) * cellSize;
                parameter.resolution[split].x = cellSize;
                parameter.resolution[split].y = cellSize;
            }
            // 阴影相机同样按视口位裁剪，避免被距离裁剪掉的图层继续投影。
            Ogre::ShadowNodeHelper::createShadowNodeWithSettings(
                compositor, capabilities, m_shadowNodeNames[index],
                Ogre::ShadowNodeHelper::ShadowParamVec{parameter}, false,
                1024u, 0.95f, 1.0f, 0.125f, 0.313f, 0u, viewVisibilityMask());
        }
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_SHADOW_NODE_FAILED: "
                  << error.getFullDescription() << "\n";
        destroyShadowNodeDefinitions();
        return false;
    }
}

void OgreViewportSurface::destroyShadowNodeDefinitions()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root) return;
    Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
    for (const std::string& name : m_shadowNodeNames) {
        if (!name.empty() && compositor->hasShadowNodeDefinition(Ogre::IdString(name)))
            compositor->removeShadowNodeDefinition(Ogre::IdString(name));
    }
}

bool OgreViewportSurface::createViewportWorkspaceDefinitions()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root || m_backgroundMaterialName.empty()) return false;
    m_shadowViewportNodeNames = {"View3DViewportNodePerformance" + m_instanceSuffix,
                                 "View3DViewportNodeBalanced" + m_instanceSuffix,
                                 "View3DViewportNodeQuality" + m_instanceSuffix};
    m_shadowWorkspaceNames = {"View3DViewportWorkspacePerformance" + m_instanceSuffix,
                              "View3DViewportWorkspaceBalanced" + m_instanceSuffix,
                              "View3DViewportWorkspaceQuality" + m_instanceSuffix};
    try {
        Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
        const auto addDefinition = [this, compositor](
                                       const std::string& nodeName,
                                       const std::string& workspaceName,
                                       const std::string& shadowNodeName) {
            if (compositor->hasWorkspaceDefinition(Ogre::IdString(workspaceName))) return;
            Ogre::CompositorNodeDef* node = compositor->addNodeDefinition(nodeName);
            node->addTextureSourceName("rt_renderwindow", 0,
                                       Ogre::TextureDefinitionBase::TEXTURE_INPUT);
            node->setNumTargetPass(1);
            Ogre::CompositorTargetDef* target = node->addTargetPass("rt_renderwindow");
            target->setNumPasses(3);
            auto* clearPass = static_cast<Ogre::CompositorPassClearDef*>(
                target->addPass(Ogre::PASS_CLEAR));
            clearPass->setAllClearColours(Ogre::ColourValue(
                m_backgroundStyle.bottomRed, m_backgroundStyle.bottomGreen,
                m_backgroundStyle.bottomBlue, 1.0f));
            auto* quadPass = static_cast<Ogre::CompositorPassQuadDef*>(
                target->addPass(Ogre::PASS_QUAD));
            quadPass->mMaterialName = m_backgroundMaterialName;
            quadPass->mMaterialIsHlms = false;
            auto* scenePass = static_cast<Ogre::CompositorPassSceneDef*>(
                target->addPass(Ogre::PASS_SCENE));
            scenePass->setAllLoadActions(Ogre::LoadAction::Load);
            // 每个视口只看到自己那份叠加层与通过距离裁剪的图层。
            scenePass->setVisibilityMask(viewVisibilityMask());
            if (!shadowNodeName.empty()) scenePass->mShadowNode = shadowNodeName;
            Ogre::CompositorWorkspaceDef* workspace =
                compositor->addWorkspaceDefinition(workspaceName);
            workspace->connectExternal(0, Ogre::IdString(nodeName), 0);
        };
        addDefinition(m_backgroundNodeName, m_backgroundWorkspaceName, {});
        for (std::size_t index = 0; index < m_shadowNodeNames.size(); ++index)
            addDefinition(m_shadowViewportNodeNames[index], m_shadowWorkspaceNames[index],
                          m_shadowNodeNames[index]);
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_VIEWPORT_DEFINITION_FAILED: "
                  << error.getFullDescription() << "\n";
        destroyViewportWorkspaceDefinitions();
        return false;
    }
}

void OgreViewportSurface::destroyViewportWorkspaceDefinitions()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root) return;
    Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
    const auto removeDefinition = [compositor](const std::string& nodeName,
                                                const std::string& workspaceName) {
        if (!workspaceName.empty() &&
            compositor->hasWorkspaceDefinition(Ogre::IdString(workspaceName)))
            compositor->removeWorkspaceDefinition(Ogre::IdString(workspaceName));
        if (!nodeName.empty() && compositor->hasNodeDefinition(Ogre::IdString(nodeName)))
            compositor->removeNodeDefinition(Ogre::IdString(nodeName));
    };
    removeDefinition(m_backgroundNodeName, m_backgroundWorkspaceName);
    for (std::size_t index = 0; index < m_shadowViewportNodeNames.size(); ++index)
        removeDefinition(m_shadowViewportNodeNames[index], m_shadowWorkspaceNames[index]);
}

bool OgreViewportSurface::switchViewportWorkspace(bool enabled,
                                                  ViewportShadowQuality quality)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root || !m_sceneManager || !m_window || !m_camera) return false;
    const ShadowPreset preset = shadowPreset(quality);
    std::size_t qualityIndex = 1u;
    if (quality == ViewportShadowQuality::Performance) qualityIndex = 0u;
    else if (quality == ViewportShadowQuality::Quality) qualityIndex = 2u;
    const std::string& workspaceName = enabled
        ? m_shadowWorkspaceNames[qualityIndex] : m_backgroundWorkspaceName;
    Ogre::CompositorWorkspace* replacement = nullptr;
    try {
        Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
        replacement = compositor->addWorkspace(
            m_sceneManager, m_window->getTexture(), m_camera,
            Ogre::IdString(workspaceName), false);
        if (!replacement) return false;
        Ogre::Hlms* hlms = m_root->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        if (!hlms) {
            compositor->removeWorkspace(replacement);
            return false;
        }
        Ogre::HlmsPbs* pbs = static_cast<Ogre::HlmsPbs*>(hlms);
        Ogre::HlmsPbs::ShadowFilter shadowFilter = Ogre::HlmsPbs::PCF_3x3;
        if (preset.filter == ViewportShadowFilter::Pcf2x2)
            shadowFilter = Ogre::HlmsPbs::PCF_2x2;
        else if (preset.filter == ViewportShadowFilter::Pcf4x4)
            shadowFilter = Ogre::HlmsPbs::PCF_4x4;
        pbs->setShadowSettings(shadowFilter);
        if (m_workspace) m_workspace->setEnabled(false);
        replacement->setEnabled(renderActive());
        Ogre::CompositorWorkspace* previous = m_workspace;
        m_workspace = replacement;
        if (previous) compositor->removeWorkspace(previous);
        m_shadowRuntime.enabled = enabled;
        m_shadowRuntime.requestedQuality = quality;
        m_shadowRuntime.actualQuality = quality;
        m_shadowRuntime.filter = preset.filter;
        m_shadowRuntime.splitCount = preset.splitCount;
        m_shadowRuntime.resolution = preset.resolution;
        m_shadowRuntime.workspaceUsesShadowNode = enabled;
        m_shadowRuntime.activeWorkspaceCount = 1u;
        ++m_shadowRuntime.workspaceSwitchCount;
        return true;
    } catch (const Ogre::Exception& error) {
        if (replacement && m_root)
            m_root->getCompositorManager2()->removeWorkspace(replacement);
        if (m_workspace) m_workspace->setEnabled(true);
        std::cerr << "VIEW3D_VIEWPORT_SWITCH_FAILED: "
                  << error.getFullDescription() << "\n";
        return false;
    }
}

void OgreViewportSurface::destroyBackgroundResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_root) return;
    if (!m_backgroundMaterialName.empty())
        Ogre::MaterialManager::getSingleton().remove(m_backgroundMaterialName);
    Ogre::HighLevelGpuProgramManager& programManager =
        Ogre::HighLevelGpuProgramManager::getSingleton();
    if (!m_backgroundFragmentProgramName.empty())
        programManager.remove(m_backgroundFragmentProgramName);
    if (!m_backgroundVertexProgramName.empty())
        programManager.remove(m_backgroundVertexProgramName);
    m_backgroundWorkspaceName.clear();
    m_backgroundNodeName.clear();
    m_backgroundMaterialName.clear();
    m_backgroundFragmentProgramName.clear();
    m_backgroundVertexProgramName.clear();
}



bool OgreViewportSurface::setShadowStyle(const ViewportShadowStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const ViewportShadowStyle previousStyle = m_shadowStyle;
    const ViewportShadowRuntimeState previousRuntime = m_shadowRuntime;
    m_shadowStyle = style;
    if (m_sceneManager) {
        if (switchViewportWorkspace(style.enabled, style.quality))
            return true;
        switchViewportWorkspace(previousStyle.enabled, previousStyle.quality);
        m_shadowStyle = previousStyle;
        m_shadowRuntime = previousRuntime;
        return false;
    }
    const ShadowPreset preset = shadowPreset(style.quality);
    m_shadowRuntime.enabled = style.enabled;
    m_shadowRuntime.requestedQuality = style.quality;
    m_shadowRuntime.actualQuality = style.quality;
    m_shadowRuntime.filter = preset.filter;
    m_shadowRuntime.splitCount = preset.splitCount;
    m_shadowRuntime.resolution = preset.resolution;
    return true;
}

bool OgreViewportSurface::setShadowsEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ViewportShadowStyle style = m_shadowStyle;
    style.enabled = enabled;
    return setShadowStyle(style);
}

bool OgreViewportSurface::setShadowQuality(ViewportShadowQuality quality)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ViewportShadowStyle style = m_shadowStyle;
    style.quality = quality;
    return setShadowStyle(style);
}

bool OgreViewportSurface::applyClippingState(const ViewportClippingState& state)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    double lengthSquared = 0.0;
    for (double value : state.normal) {
        if (!std::isfinite(value)) return false;
        lengthSquared += value * value;
    }
    if (!std::isfinite(state.offset)) return false;
    if (state.enabled && lengthSquared < 1e-12) return false;
    m_clippingState = state;
    rebuildClippingHelper();
    if (!state.enabled) {
        m_camera->disableCustomNearClipPlane();
        return true;
    }
    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    Ogre::Vector3 normal(
        static_cast<Ogre::Real>(state.normal[0] * inverseLength),
        static_cast<Ogre::Real>(state.normal[1] * inverseLength),
        static_cast<Ogre::Real>(state.normal[2] * inverseLength));
    Ogre::Real distance = static_cast<Ogre::Real>(-state.offset * inverseLength);
    if (state.inverted) {
        normal = -normal;
        distance = -distance;
    }
    m_camera->enableCustomNearClipPlane(Ogre::Plane(normal, distance));
    return true;
}

void OgreViewportSurface::clearClippingHelperResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && m_clippingHelperObject)
        m_sceneManager->destroyManualObject(m_clippingHelperObject);
    m_clippingHelperObject = nullptr;
    if (m_root && m_clippingHelperDatablock && !m_clippingHelperDatablockName.empty()) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit)
            unlit->destroyDatablock(Ogre::IdString(m_clippingHelperDatablockName));
    }
    m_clippingHelperDatablock = nullptr;
    m_clippingHelperDatablockName.clear();
}

bool OgreViewportSurface::rebuildClippingHelper()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearClippingHelperResources();
    if (!m_clippingState.enabled || !m_clippingState.helperVisible || !m_sceneManager ||
        !m_root)
        return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit)
            return false;
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = false;
        m_clippingHelperDatablockName = "View3DClippingHelper" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_clippingHelperDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_clippingHelperDatablockName,
                                   m_clippingHelperDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        m_clippingHelperDatablock->setUseColour(true);
        m_clippingHelperDatablock->setColour(
            Ogre::ColourValue(0.98f, 0.55f, 0.12f, 0.28f));

        double lengthSquared = 0.0;
        for (double value : m_clippingState.normal)
            lengthSquared += value * value;
        if (lengthSquared < 1e-12)
            return true;
        const double inverseLength = 1.0 / std::sqrt(lengthSquared);
        Ogre::Vector3 normal(
            static_cast<Ogre::Real>(m_clippingState.normal[0] * inverseLength),
            static_cast<Ogre::Real>(m_clippingState.normal[1] * inverseLength),
            static_cast<Ogre::Real>(m_clippingState.normal[2] * inverseLength));
        const double distance = m_clippingState.offset * inverseLength;
        if (m_clippingState.inverted)
            normal = -normal;
        const Ogre::Vector3 center = normal * static_cast<Ogre::Real>(distance);
        Ogre::Vector3 tangent = normal.crossProduct(Ogre::Vector3::UNIT_Y);
        if (tangent.squaredLength() < 1e-6f)
            tangent = normal.crossProduct(Ogre::Vector3::UNIT_X);
        tangent.normalise();
        const Ogre::Vector3 bitangent = normal.crossProduct(tangent).normalisedCopy();
        const Ogre::Real size = 200.0f;
        m_clippingHelperObject = m_sceneManager->createManualObject();
        m_clippingHelperObject->setCastShadows(false);
        m_clippingHelperObject->setVisibilityFlags(viewVisibilityMask());
        m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
            ->attachObject(m_clippingHelperObject);
        m_clippingHelperObject->begin(m_clippingHelperDatablockName, Ogre::OT_TRIANGLE_LIST);
        const Ogre::Vector3 corners[4] = {
            center - tangent * size - bitangent * size,
            center + tangent * size - bitangent * size,
            center + tangent * size + bitangent * size,
            center - tangent * size + bitangent * size};
        for (int i = 0; i < 4; ++i) {
            m_clippingHelperObject->position(corners[i]);
            m_clippingHelperObject->colour(0.98f, 0.55f, 0.12f, 0.28f);
            m_clippingHelperObject->index(static_cast<Ogre::uint32>(i));
        }
        m_clippingHelperObject->index(0);
        m_clippingHelperObject->index(2);
        m_clippingHelperObject->end()->setDatablock(m_clippingHelperDatablock);
        return true;
    } catch (const Ogre::Exception&) {
        clearClippingHelperResources();
        return false;
    }
}



bool OgreViewportSurface::cameraState(ViewportCameraState* state) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!state || !m_camera || width() <= 0 || height() <= 0) return false;
    if (m_globeMode) {
        // 球面状态按 ECEF 双精度给出；direction/up 是单位向量，与平移锚点无关。
        if (!m_globeCameraValid) return false;
        const Ogre::Vector3 direction = m_camera->getDerivedDirection();
        const Ogre::Vector3 up = m_camera->getDerivedUp();
        state->position = m_globePositionEcef;
        state->target = m_globeTargetEcef;
        state->direction = {direction.x, direction.y, direction.z};
        state->up = {up.x, up.y, up.z};
        state->viewport = {static_cast<double>(width()), static_cast<double>(height())};
        state->orthographic = false;
        state->orthographicHeight = 0.0;
        state->verticalFovRadians = m_camera->getFOVy().valueRadians();
        state->horizontalFovRadians = 2.0 * std::atan(
            std::tan(state->verticalFovRadians * 0.5) * m_camera->getAspectRatio());
        return true;
    }
    const Ogre::Vector3 position = m_camera->getDerivedPosition();
    const Ogre::Vector3 direction = m_camera->getDerivedDirection();
    const Ogre::Vector3 up = m_camera->getDerivedUp();
    state->position = {position.x, position.y, position.z};
    state->direction = {direction.x, direction.y, direction.z};
    state->up = {up.x, up.y, up.z};
    state->target = m_cameraTarget;
    state->viewport = {static_cast<double>(width()), static_cast<double>(height())};
    state->orthographic =
        m_camera->getProjectionType() == Ogre::ProjectionType::PT_ORTHOGRAPHIC;
    state->orthographicHeight = state->orthographic ? m_camera->getOrthoWindowHeight() : 0.0;
    if (state->orthographic) {
        const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                                   static_cast<Ogre::Real>(m_cameraTarget[1]),
                                   static_cast<Ogre::Real>(m_cameraTarget[2]));
        const double distance = qMax<double>(
            (m_camera->getDerivedPosition() - target).length(), 0.1);
        state->verticalFovRadians = 2.0 * std::atan(state->orthographicHeight / (2.0 * distance));
    } else {
        state->verticalFovRadians = m_camera->getFOVy().valueRadians();
    }
    state->horizontalFovRadians = 2.0 * std::atan(
        std::tan(state->verticalFovRadians * 0.5) * m_camera->getAspectRatio());
    return true;
}

bool OgreViewportSurface::setCameraProjectionMode(CameraProjectionMode mode)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    // 球面只支持透视：正交投影在地球尺度上没有意义的视锥。
    if (m_globeMode) return mode == CameraProjectionMode::Perspective;
    if (mode == CameraProjectionMode::Orthographic) {
        const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                                   static_cast<Ogre::Real>(m_cameraTarget[1]),
                                   static_cast<Ogre::Real>(m_cameraTarget[2]));
        const Ogre::Real distance = qMax<Ogre::Real>(
            (m_camera->getDerivedPosition() - target).length(), 0.1f);
        const Ogre::Real height = 2.0f * distance *
            std::tan(m_camera->getFOVy().valueRadians() * 0.5f);
        m_camera->setProjectionType(Ogre::ProjectionType::PT_ORTHOGRAPHIC);
        m_camera->setOrthoWindowHeight(qMax<Ogre::Real>(height, 0.01f));
    } else {
        m_camera->setProjectionType(Ogre::ProjectionType::PT_PERSPECTIVE);
    }
    return true;
}

CameraProjectionMode OgreViewportSurface::cameraProjectionMode() const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_camera && m_camera->getProjectionType() == Ogre::ProjectionType::PT_ORTHOGRAPHIC
        ? CameraProjectionMode::Orthographic
        : CameraProjectionMode::Perspective;
}

bool OgreViewportSurface::setStandardCameraView(StandardCameraView view)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    // 球面没有世界轴意义上的立面视角。
    if (m_globeMode) return false;
    // 二维下只有俯视图有意义，其余立面视角一律拒绝（避免破坏锁定姿态）。
    if (m_planarMode && view != StandardCameraView::Top) return false;
    const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                               static_cast<Ogre::Real>(m_cameraTarget[1]),
                               static_cast<Ogre::Real>(m_cameraTarget[2]));
    Ogre::Real distance = (m_camera->getDerivedPosition() - target).length();
    if (distance < 0.1f) distance = 10.0f;
    Ogre::Vector3 offset;
    Ogre::Vector3 up = Ogre::Vector3::UNIT_Y;
    switch (view) {
    case StandardCameraView::Front: offset = Ogre::Vector3::UNIT_Z; break;
    case StandardCameraView::Back: offset = Ogre::Vector3::NEGATIVE_UNIT_Z; break;
    case StandardCameraView::Left: offset = Ogre::Vector3::NEGATIVE_UNIT_X; break;
    case StandardCameraView::Right: offset = Ogre::Vector3::UNIT_X; break;
    case StandardCameraView::Top:
        offset = Ogre::Vector3::UNIT_Y;
        up = Ogre::Vector3::NEGATIVE_UNIT_Z;
        break;
    case StandardCameraView::Bottom:
        offset = Ogre::Vector3::NEGATIVE_UNIT_Y;
        up = Ogre::Vector3::UNIT_Z;
        break;
    case StandardCameraView::Isometric:
        offset = Ogre::Vector3(1.0f, 1.0f, 1.0f).normalisedCopy();
        break;
    }
    m_camera->setPosition(target + offset * distance);
    m_camera->setFixedYawAxis(true, up);
    m_camera->setDirection(-offset);
    return true;
}

bool OgreViewportSurface::setCameraState(const ViewportCameraState& state)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    if (m_globeMode) return applyGlobeCameraState(state);
    if (m_planarMode) return applyPlanarCameraState(state);
    Ogre::Vector3 direction(static_cast<Ogre::Real>(state.direction[0]),
                            static_cast<Ogre::Real>(state.direction[1]),
                            static_cast<Ogre::Real>(state.direction[2]));
    Ogre::Vector3 up(static_cast<Ogre::Real>(state.up[0]),
                     static_cast<Ogre::Real>(state.up[1]),
                     static_cast<Ogre::Real>(state.up[2]));
    if (direction.squaredLength() < 0.000001f || up.squaredLength() < 0.000001f)
        return false;
    direction.normalise();
    up.normalise();
    if (qAbs(direction.dotProduct(up)) > 0.999f) return false;
    m_cameraTarget = state.target;
    m_camera->setPosition(static_cast<Ogre::Real>(state.position[0]),
                          static_cast<Ogre::Real>(state.position[1]),
                          static_cast<Ogre::Real>(state.position[2]));
    m_camera->setFixedYawAxis(true, up);
    m_camera->setDirection(direction);
    if (state.verticalFovRadians > 0.01 && state.verticalFovRadians < 3.13)
        m_camera->setFOVy(Ogre::Radian(static_cast<Ogre::Real>(state.verticalFovRadians)));
    m_camera->setProjectionType(state.orthographic
        ? Ogre::ProjectionType::PT_ORTHOGRAPHIC
        : Ogre::ProjectionType::PT_PERSPECTIVE);
    if (state.orthographic)
        m_camera->setOrthoWindowHeight(
            static_cast<Ogre::Real>(qMax(state.orthographicHeight, 0.01)));
    updateClipDistances((m_camera->getDerivedPosition() -
                         Ogre::Vector3(static_cast<Ogre::Real>(state.target[0]),
                                       static_cast<Ogre::Real>(state.target[1]),
                                       static_cast<Ogre::Real>(state.target[2]))).length());
    return true;
}

bool OgreViewportSurface::setPlanarMode(bool enabled)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    if (enabled == m_planarMode) return true;
    // 球面与二维互斥：编排层负责先退出球面再进二维。
    if (m_globeMode) return false;
    if (enabled) {
        // 进入必须原子：读不到基准快照就整体失败。否则会出现"已经压平成
        // 正俯视+正交，但没有还原依据"的半态，界面显示三维而画面停在二维。
        ViewportCameraState snapshot;
        if (!cameraState(&snapshot)) return false;
        m_planarReturnState = snapshot;
        m_planarReturnValid = true;
        m_planarMode = true;
        // 二维按地图口径看：正交投影 + 正俯视，屏幕横轴=东(X)、纵轴=北(-Z)。
        setCameraProjectionMode(CameraProjectionMode::Orthographic);
        applyPlanarPose();
        return true;
    }
    m_planarMode = false;
    if (!m_planarReturnValid) return true;
    const ViewportCameraState previous = m_planarReturnState;
    m_planarReturnValid = false;
    return restoreFromPlanar(previous);
}

bool OgreViewportSurface::restoreFromPlanar(const ViewportCameraState& previous)
{
    // 原地起身：观察点和缩放跟着二维里的操作走，俯仰/方位/投影回到进入前的风格。
    ViewportCameraState current;
    if (!cameraState(&current)) return setCameraState(previous);
    // 视线方向必须用快照里的 direction：target 只是轨道枢轴，可能不在视线轴上，
    // 用 position-target 反推会得到另一个朝向（斜视时尤其明显）。
    Ogre::Vector3 direction(static_cast<Ogre::Real>(previous.direction[0]),
                            static_cast<Ogre::Real>(previous.direction[1]),
                            static_cast<Ogre::Real>(previous.direction[2]));
    if (direction.squaredLength() < 1e-8f) {
        direction = Ogre::Vector3(static_cast<Ogre::Real>(previous.target[0] - previous.position[0]),
                                  static_cast<Ogre::Real>(previous.target[1] - previous.position[1]),
                                  static_cast<Ogre::Real>(previous.target[2] - previous.position[2]));
    }
    if (direction.squaredLength() < 1e-8f) direction = Ogre::Vector3(0.0f, -0.7f, -0.7f);
    direction.normalise();
    Ogre::Real distance = static_cast<Ogre::Real>(current.position[1] - current.target[1]);
    if (previous.orthographic) {
        // 正交→正交：缩放就是可见高度，直接沿用二维里的值。
        if (!(current.orthographicHeight > 0.01)) distance = qMax<Ogre::Real>(distance, 0.1f);
    } else if (current.orthographicHeight > 0.01 && previous.verticalFovRadians > 0.01 &&
        previous.verticalFovRadians < 3.13) {
        // 正交→透视：把二维的可见高度折算成同等视野的观察距离，缩放感不被抹掉。
        distance = static_cast<Ogre::Real>(
            current.orthographicHeight / (2.0 * std::tan(previous.verticalFovRadians * 0.5)));
    }
    if (!std::isfinite(distance) || distance < 0.1f) distance = 10.0f;
    const Ogre::Vector3 target(static_cast<Ogre::Real>(current.target[0]),
                               static_cast<Ogre::Real>(current.target[1]),
                               static_cast<Ogre::Real>(current.target[2]));
    // setCameraState 会直接拒绝 direction 与 up 接近平行的姿态。快照里若出现这种
    // 组合（进入前就是正俯视、或上游改过 up），整段还原会被静默丢掉：模式标记已翻回
    // 三维、相机却还停在二维。这里先把 up 正交化，实在不行再挑一根最不平行的世界轴。
    Ogre::Vector3 up(static_cast<Ogre::Real>(previous.up[0]),
                     static_cast<Ogre::Real>(previous.up[1]),
                     static_cast<Ogre::Real>(previous.up[2]));
    if (up.squaredLength() < 1e-8f) up = Ogre::Vector3::UNIT_Y;
    up = up - direction * up.dotProduct(direction);
    if (up.squaredLength() < 1e-6f) {
        const Ogre::Vector3 candidates[3] = {Ogre::Vector3::UNIT_X, Ogre::Vector3::UNIT_Y,
                                             Ogre::Vector3::UNIT_Z};
        up = Ogre::Vector3::UNIT_Y;
        double best = -1.0;
        for (const Ogre::Vector3& candidate : candidates) {
            const double alignment = qAbs(candidate.dotProduct(direction));
            if (best < 0.0 || alignment < best) {
                best = alignment;
                up = candidate;
            }
        }
        up = up - direction * up.dotProduct(direction);
    }
    if (up.squaredLength() < 1e-8f) up = Ogre::Vector3(0.0f, 0.0f, 1.0f);
    up.normalise();
    ViewportCameraState restored = previous;
    restored.target = current.target;
    restored.position = {target.x - direction.x * distance, target.y - direction.y * distance,
                         target.z - direction.z * distance};
    restored.direction = {direction.x, direction.y, direction.z};
    restored.up = {up.x, up.y, up.z};
    restored.orthographic = previous.orthographic;
    if (previous.orthographic && current.orthographicHeight > 0.01)
        restored.orthographicHeight = current.orthographicHeight;
    if (!setCameraState(restored)) {
        // 静默失败会变成"显示三维、画面还是俯视"，必须留一条可对账的线索。
        std::cerr << "VIEW3D_PLANAR_RESTORE_FAILED up=" << up.x << "," << up.y << "," << up.z
                  << " direction=" << direction.x << "," << direction.y << "," << direction.z
                  << std::endl;
        return false;
    }
    return true;
}

void OgreViewportSurface::applyPlanarPose()
{
    if (!m_camera) return;
    const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                               static_cast<Ogre::Real>(m_cameraTarget[1]),
                               static_cast<Ogre::Real>(m_cameraTarget[2]));
    Ogre::Real distance = (m_camera->getDerivedPosition() - target).length();
    if (!std::isfinite(distance) || distance < 0.1f) distance = 10.0f;
    m_camera->setFixedYawAxis(true, Ogre::Vector3::NEGATIVE_UNIT_Z);
    m_camera->setPosition(target + Ogre::Vector3::UNIT_Y * distance);
    m_camera->setDirection(Ogre::Vector3::NEGATIVE_UNIT_Y);
    updateClipDistances(static_cast<double>(distance));
}

bool OgreViewportSurface::applyPlanarCameraState(const ViewportCameraState& state)
{
    if (!m_camera) return false;
    const Ogre::Vector3 target(static_cast<Ogre::Real>(state.target[0]),
                               static_cast<Ogre::Real>(state.target[1]),
                               static_cast<Ogre::Real>(state.target[2]));
    const Ogre::Vector3 wanted(static_cast<Ogre::Real>(state.position[0]),
                               static_cast<Ogre::Real>(state.position[1]),
                               static_cast<Ogre::Real>(state.position[2]));
    Ogre::Real distance = (wanted - target).length();
    if (!std::isfinite(distance) || distance < 0.1f) distance = 10.0f;
    m_cameraTarget = state.target;
    m_camera->setProjectionType(Ogre::ProjectionType::PT_ORTHOGRAPHIC);
    if (state.orthographic && state.orthographicHeight > 0.01)
        m_camera->setOrthoWindowHeight(static_cast<Ogre::Real>(state.orthographicHeight));
    m_camera->setFixedYawAxis(true, Ogre::Vector3::NEGATIVE_UNIT_Z);
    m_camera->setPosition(target + Ogre::Vector3::UNIT_Y * distance);
    m_camera->setDirection(Ogre::Vector3::NEGATIVE_UNIT_Y);
    updateClipDistances(static_cast<double>(distance));
    return true;
}

bool OgreViewportSurface::setGlobeModeEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    if (enabled == m_globeMode) return true;
    if (enabled && m_planarMode) return false;
    m_globeMode = enabled;
    // ECEF 真值由编排层紧接着的 setCameraState 写入；在此之前不动锚点。
    m_globeCameraValid = false;
    if (enabled)
        m_camera->setProjectionType(Ogre::ProjectionType::PT_PERSPECTIVE);
    return true;
}

bool OgreViewportSurface::globeModeEnabled() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_globeMode;
}

bool OgreViewportSurface::applyGlobeCameraState(const ViewportCameraState& state)
{
    if (!m_camera) return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(state.position[axis]) || !std::isfinite(state.target[axis]) ||
            !std::isfinite(state.direction[axis]) || !std::isfinite(state.up[axis]))
            return false;
    }
    Ogre::Vector3 direction(static_cast<Ogre::Real>(state.direction[0]),
                            static_cast<Ogre::Real>(state.direction[1]),
                            static_cast<Ogre::Real>(state.direction[2]));
    Ogre::Vector3 up(static_cast<Ogre::Real>(state.up[0]),
                     static_cast<Ogre::Real>(state.up[1]),
                     static_cast<Ogre::Real>(state.up[2]));
    if (direction.squaredLength() < 1e-12f || up.squaredLength() < 1e-12f) return false;
    direction.normalise();
    up.normalise();
    if (qAbs(direction.dotProduct(up)) > 0.999f) return false;
    m_globePositionEcef = state.position;
    m_globeTargetEcef = state.target;
    m_globeCameraValid = true;
    m_camera->setProjectionType(Ogre::ProjectionType::PT_PERSPECTIVE);
    if (state.verticalFovRadians > 0.01 && state.verticalFovRadians < 3.13)
        m_camera->setFOVy(Ogre::Radian(static_cast<Ogre::Real>(state.verticalFovRadians)));
    m_camera->setDirection(direction);
    return applyGlobeCamera();
}

bool OgreViewportSurface::applyGlobeCamera()
{
    if (!m_camera || !m_globeCameraValid) return false;
    // 锚点=本相机 ECEF：相减后相机落在原点，瓦片/椭球的 float 精度最优。
    m_scene.setGlobeRenderOrigin(m_globePositionEcef);
    m_camera->setPosition(Ogre::Vector3::ZERO);
    Ogre::Vector3 direction = m_camera->getDerivedDirection();
    if (direction.squaredLength() < 1e-8f) {
        const GlobeMath::Vec3 toward = GlobeMath::sub(m_globeTargetEcef, m_globePositionEcef);
        direction = Ogre::Vector3(static_cast<Ogre::Real>(toward[0]),
                                  static_cast<Ogre::Real>(toward[1]),
                                  static_cast<Ogre::Real>(toward[2]));
        if (direction.squaredLength() < 1e-8f) direction = Ogre::Vector3(0.0f, 0.0f, -1.0f);
        direction.normalise();
    }
    // up 始终取相机处的椭球法线再对视线正交化：地平线不随滚转累积。
    const GlobeMath::Vec3 normal = GlobeMath::ecefNormal(m_globePositionEcef);
    Ogre::Vector3 up(static_cast<Ogre::Real>(normal[0]), static_cast<Ogre::Real>(normal[1]),
                     static_cast<Ogre::Real>(normal[2]));
    if (up.squaredLength() < 1e-8f) up = Ogre::Vector3::UNIT_Y;
    up.normalise();
    up = up - direction * up.dotProduct(direction);
    if (up.squaredLength() < 1e-6f) {
        // 正对天底/天顶：法线与视线平行，挑最不平行的世界轴做地平线基准。
        const Ogre::Vector3 candidates[3] = {Ogre::Vector3::UNIT_X, Ogre::Vector3::UNIT_Y,
                                             Ogre::Vector3::UNIT_Z};
        double best = -1.0;
        for (const Ogre::Vector3& candidate : candidates) {
            const double alignment = qAbs(candidate.dotProduct(direction));
            if (best < 0.0 || alignment < best) {
                best = alignment;
                up = candidate;
            }
        }
        up = up - direction * up.dotProduct(direction);
    }
    if (up.squaredLength() < 1e-8f) return false;
    up.normalise();
    m_camera->setFixedYawAxis(true, up);
    m_camera->setDirection(direction);
    m_cameraTarget = GlobeMath::sub(m_globeTargetEcef, m_globePositionEcef);
    updateGlobeClipDistances();
    return true;
}

void OgreViewportSurface::syncGlobeCamera()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_globeMode || !m_globeCameraValid || !m_camera) return;
    std::array<double, 3> anchor {};
    if (!m_scene.globeRenderOrigin(&anchor)) return;
    // 多视口时锚点由最后一个 prepareFrame 决定；这里把本相机摆回最终锚点。
    const GlobeMath::Vec3 relative = GlobeMath::sub(m_globePositionEcef, anchor);
    m_camera->setPosition(static_cast<Ogre::Real>(relative[0]),
                          static_cast<Ogre::Real>(relative[1]),
                          static_cast<Ogre::Real>(relative[2]));
    m_cameraTarget = GlobeMath::sub(m_globeTargetEcef, anchor);
    updateGlobeClipDistances();
}

void OgreViewportSurface::updateGlobeClipDistances()
{
    if (!m_camera || !m_globeCameraValid) return;
    const double altitude = qMax(0.0, GlobeMath::ecefHeight(m_globePositionEcef));
    const double distance =
        GlobeMath::length(GlobeMath::sub(m_globePositionEcef, m_globeTargetEcef));
    // 近裁剪跟随高度/观察距离：太大切掉贴近的瓦片，太小在地球尺度浪费深度精度。
    const double nearDistance = std::clamp(qMax(altitude, distance) * 0.001, 0.1, 10000.0);
    // 远裁剪必须罩住整个椭球（直径约 1.27e7 m）。
    const double farDistance = std::max({1.0e8, distance * 4.0, altitude * 4.0});
    m_camera->setNearClipDistance(static_cast<Ogre::Real>(nearDistance));
    m_camera->setFarClipDistance(static_cast<Ogre::Real>(farDistance));
}

void OgreViewportSurface::clampGlobeCameraAltitude()
{
    if (!m_globeCameraValid) return;
    double lon = 0.0;
    double lat = 0.0;
    double height = 0.0;
    if (!GlobeMath::ecefToGeodetic(m_globePositionEcef, &lon, &lat, &height)) return;
    if (height >= 1.0) return;
    m_globePositionEcef = GlobeMath::geodeticToEcef(lon, lat, 1.0);
}

bool OgreViewportSurface::globeOrbit(double yawDegrees, double pitchDegrees)
{
    if (!std::isfinite(yawDegrees) || !std::isfinite(pitchDegrees)) return false;
    const GlobeMath::Vec3 pivot = m_globeTargetEcef;
    const GlobeMath::Vec3 offset = GlobeMath::sub(m_globePositionEcef, pivot);
    if (GlobeMath::length(offset) < 1e-6) return false;
    const GlobeMath::Vec3 normal = GlobeMath::ecefNormal(pivot);
    if (GlobeMath::length(normal) < 1e-9) return false;
    // 全程双精度 Rodrigues 旋转：地球尺度的 offset 走 float 四元数会抖米级。
    const auto rotateAround = [](const GlobeMath::Vec3& v, const GlobeMath::Vec3& axis,
                                 double radians) {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        return GlobeMath::add(
            GlobeMath::add(GlobeMath::scale(v, c),
                           GlobeMath::scale(GlobeMath::cross(axis, v), s)),
            GlobeMath::scale(axis, GlobeMath::dot(axis, v) * (1.0 - c)));
    };
    const double yawRadians = yawDegrees * GlobeMath::kRadiansPerDegree;
    const double pitchRadians = pitchDegrees * GlobeMath::kRadiansPerDegree;
    const Ogre::Vector3 right = m_camera->getDerivedRight();
    GlobeMath::Vec3 pitchAxis =
        GlobeMath::normalized(rotateAround({right.x, right.y, right.z}, normal, yawRadians));
    if (GlobeMath::length(pitchAxis) < 1e-9)
        pitchAxis = GlobeMath::normalized({right.x, right.y, right.z});
    GlobeMath::Vec3 candidate = rotateAround(offset, normal, yawRadians);
    candidate = rotateAround(candidate, pitchAxis, -pitchRadians);
    const Ogre::Vector3 currentDirection = m_camera->getDerivedDirection();
    GlobeMath::Vec3 direction = rotateAround(
        {currentDirection.x, currentDirection.y, currentDirection.z}, normal, yawRadians);
    direction = rotateAround(direction, pitchAxis, -pitchRadians);
    const GlobeMath::Vec3 newPosition = GlobeMath::add(pivot, candidate);
    // 不钻到椭球面以下；沿法线朝天看没有地平线意义，拒绝（天底俯视允许）。
    if (GlobeMath::ecefHeight(newPosition) < 1.0) return true;
    if (GlobeMath::dot(direction, GlobeMath::ecefNormal(newPosition)) > 0.995) return true;
    m_globePositionEcef = newPosition;
    m_camera->setDirection(Ogre::Vector3(static_cast<Ogre::Real>(direction[0]),
                                         static_cast<Ogre::Real>(direction[1]),
                                         static_cast<Ogre::Real>(direction[2])));
    return applyGlobeCamera();
}

bool OgreViewportSurface::globePan(double horizontalPixels, double verticalPixels)
{
    if (!std::isfinite(horizontalPixels) || !std::isfinite(verticalPixels)) return false;
    const double distance =
        GlobeMath::length(GlobeMath::sub(m_globePositionEcef, m_globeTargetEcef));
    const double viewHeight =
        2.0 * qMax(distance, 1.0) * std::tan(m_camera->getFOVy().valueRadians() * 0.5);
    const double unitsPerPixel = viewHeight / height();
    const Ogre::Vector3 right = m_camera->getDerivedRight();
    const Ogre::Vector3 up = m_camera->getDerivedUp();
    const GlobeMath::Vec3 translation = GlobeMath::add(
        GlobeMath::scale({right.x, right.y, right.z}, -horizontalPixels * unitsPerPixel),
        GlobeMath::scale({up.x, up.y, up.z}, verticalPixels * unitsPerPixel));
    m_globePositionEcef = GlobeMath::add(m_globePositionEcef, translation);
    GlobeMath::Vec3 movedTarget = GlobeMath::add(m_globeTargetEcef, translation);
    double lon = 0.0;
    double lat = 0.0;
    double heightMeters = 0.0;
    if (GlobeMath::ecefToGeodetic(movedTarget, &lon, &lat, &heightMeters) &&
        heightMeters < 0.0)
        movedTarget = GlobeMath::geodeticToEcef(lon, lat, 0.0);
    m_globeTargetEcef = movedTarget;
    clampGlobeCameraAltitude();
    return applyGlobeCamera();
}

bool OgreViewportSurface::globeZoom(double wheelSteps)
{
    if (!std::isfinite(wheelSteps)) return false;
    const double factor = std::pow(0.85, wheelSteps);
    GlobeMath::Vec3 offset = GlobeMath::sub(m_globePositionEcef, m_globeTargetEcef);
    double distance = GlobeMath::length(offset);
    if (distance < 1e-6) {
        offset = GlobeMath::scale(GlobeMath::ecefNormal(m_globePositionEcef), 10.0);
        distance = 10.0;
    }
    const double newDistance = std::clamp(distance * factor, 0.5, 1.0e9);
    m_globePositionEcef = GlobeMath::add(
        m_globeTargetEcef, GlobeMath::scale(GlobeMath::normalized(offset), newDistance));
    clampGlobeCameraAltitude();
    return applyGlobeCamera();
}

bool OgreViewportSurface::globeMoveLocal(double rightUnits, double upUnits,
                                         double forwardUnits)
{
    const Ogre::Vector3 right = m_camera->getDerivedRight();
    const Ogre::Vector3 up = m_camera->getDerivedUp();
    const Ogre::Vector3 forward = m_camera->getDerivedDirection();
    const GlobeMath::Vec3 translation {
        right.x * rightUnits + up.x * upUnits + forward.x * forwardUnits,
        right.y * rightUnits + up.y * upUnits + forward.y * forwardUnits,
        right.z * rightUnits + up.z * upUnits + forward.z * forwardUnits};
    if (GlobeMath::length(translation) < 1e-7) return true;
    m_globePositionEcef = GlobeMath::add(m_globePositionEcef, translation);
    m_globeTargetEcef = GlobeMath::add(m_globeTargetEcef, translation);
    clampGlobeCameraAltitude();
    return applyGlobeCamera();
}

bool OgreViewportSurface::globeFocusBounds(const RenderBounds& bounds)
{
    if (!m_globeCameraValid || !bounds.valid) return false;
    std::array<double, 3> anchor {};
    if (!m_scene.globeRenderOrigin(&anchor)) return false;
    const GlobeMath::Vec3 center = GlobeMath::add(
        anchor, {(bounds.minimum[0] + bounds.maximum[0]) * 0.5,
                 (bounds.minimum[1] + bounds.maximum[1]) * 0.5,
                 (bounds.minimum[2] + bounds.maximum[2]) * 0.5});
    const double spanX = bounds.maximum[0] - bounds.minimum[0];
    const double spanY = bounds.maximum[1] - bounds.minimum[1];
    const double spanZ = bounds.maximum[2] - bounds.minimum[2];
    const double radius = qMax(0.1, 0.5 * std::sqrt(spanX * spanX + spanY * spanY +
                                                    spanZ * spanZ));
    const GlobeMath::Vec3 normal = GlobeMath::ecefNormal(center);
    // 东向 = 极轴(Z) × 法线；极点处退化，退回 X 轴。
    GlobeMath::Vec3 east = GlobeMath::normalized(GlobeMath::cross({0.0, 0.0, 1.0}, normal));
    if (GlobeMath::length(east) < 1e-6) east = {1.0, 0.0, 0.0};
    m_globeTargetEcef = center;
    m_globePositionEcef = GlobeMath::add(
        center, GlobeMath::add(GlobeMath::scale(normal, radius * 2.5),
                               GlobeMath::scale(east, radius * 0.35)));
    const GlobeMath::Vec3 direction =
        GlobeMath::normalized(GlobeMath::sub(m_globeTargetEcef, m_globePositionEcef));
    m_camera->setDirection(Ogre::Vector3(static_cast<Ogre::Real>(direction[0]),
                                         static_cast<Ogre::Real>(direction[1]),
                                         static_cast<Ogre::Real>(direction[2])));
    return applyGlobeCamera();
}

bool OgreViewportSurface::globeSurfacePointAt(double xPixels, double yPixels,
                                               std::array<double, 3>* ecefPoint) const
{
    if (!m_camera || !ecefPoint || width() <= 0 || height() <= 0 ||
        !std::isfinite(xPixels) || !std::isfinite(yPixels) || !m_globeCameraValid)
        return false;
    const Ogre::Ray ray = m_camera->getCameraToViewportRay(
        static_cast<Ogre::Real>(xPixels / width()),
        static_cast<Ogre::Real>(yPixels / height()));
    const Ogre::Vector3 direction = ray.getDirection();
    const GlobeMath::Vec3 unit =
        GlobeMath::normalized({direction.x, direction.y, direction.z});
    if (GlobeMath::length(unit) < 1e-9) return false;
    // 射线起点用相机 ECEF，椭球中心即 ECEF 原点：全程双精度求交。
    return GlobeMath::rayEllipsoidHit(m_globePositionEcef, unit, {0.0, 0.0, 0.0}, ecefPoint);
}

bool OgreViewportSurface::setClippingState(const ViewportClippingState& state)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return applyClippingState(state);
}

bool OgreViewportSurface::orbitCamera(double yawDegrees, double pitchDegrees)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    // 二维模式不接受旋转：姿态由 applyPlanarPose 唯一决定。
    if (m_planarMode) return false;
    if (m_globeMode)
        return m_globeCameraValid && globeOrbit(yawDegrees, pitchDegrees);
    const Ogre::Vector3 pivot(static_cast<Ogre::Real>(m_cameraTarget[0]),
                              static_cast<Ogre::Real>(m_cameraTarget[1]),
                              static_cast<Ogre::Real>(m_cameraTarget[2]));
    const Ogre::Vector3 offset = m_camera->getDerivedPosition() - pivot;
    if (offset.squaredLength() < 0.000001f) return false;
    const Ogre::Quaternion yaw(Ogre::Degree(static_cast<Ogre::Real>(yawDegrees)),
                               Ogre::Vector3::UNIT_Y);
    // 俯仰轴必须是相机自身的左轴（水平、垂直于视线），不能用"枢轴方向×Y"：
    // 点击点偏屏时后者与视线不垂直，每格俯仰都会带进一点滚转，累积成左高右低。
    Ogre::Vector3 pitchAxis = yaw * m_camera->getDerivedRight();
    if (pitchAxis.squaredLength() < 0.000001f) pitchAxis = Ogre::Vector3::UNIT_X;
    pitchAxis.normalise();
    const Ogre::Quaternion pitch(Ogre::Degree(static_cast<Ogre::Real>(pitchDegrees)),
                                 -pitchAxis);
    const Ogre::Quaternion rotation = pitch * yaw;
    Ogre::Vector3 candidate = rotation * offset;
    Ogre::Vector3 direction = rotation * m_camera->getDerivedDirection();
    if (qAbs(direction.dotProduct(Ogre::Vector3::UNIT_Y)) > 0.995f) {
        // 俯仰已到限位：只丢掉这一步的俯仰，保留水平旋转。整次拒掉会让
        // 相机一旦转到接近天顶，连左右拖动也彻底不动，看起来就是"转不动了"。
        candidate = yaw * offset;
        direction = yaw * m_camera->getDerivedDirection();
        if (qAbs(direction.dotProduct(Ogre::Vector3::UNIT_Y)) > 0.995f) return true;
    }
    // 位置和朝向一起绕枢轴旋转。不能在这里 lookAt(pivot)：点击点通常不在
    // 屏幕中心，lookAt 会把视线瞬间拽过去，表现为"开始旋转时跳一下"。
    // 固定 Y 轴重建朝向，保证地平线永远水平（滚转不累积）。
    m_camera->setPosition(pivot + candidate);
    m_camera->setFixedYawAxis(true, Ogre::Vector3::UNIT_Y);
    m_camera->setDirection(direction);
    return true;
}

bool OgreViewportSurface::panCamera(double horizontalPixels, double verticalPixels)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || height() <= 0) return false;
    if (m_globeMode)
        return m_globeCameraValid && globePan(horizontalPixels, verticalPixels);
    const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                               static_cast<Ogre::Real>(m_cameraTarget[1]),
                               static_cast<Ogre::Real>(m_cameraTarget[2]));
    const Ogre::Vector3 position = m_camera->getDerivedPosition();
    const double viewHeight = m_camera->getProjectionType() == Ogre::ProjectionType::PT_ORTHOGRAPHIC
        ? m_camera->getOrthoWindowHeight()
        : 2.0 * (position - target).length() * std::tan(m_camera->getFOVy().valueRadians() * 0.5);
    const Ogre::Real unitsPerPixel = static_cast<Ogre::Real>(viewHeight / height());
    const Ogre::Vector3 translation =
        m_camera->getDerivedRight() * static_cast<Ogre::Real>(-horizontalPixels) * unitsPerPixel +
        m_camera->getDerivedUp() * static_cast<Ogre::Real>(verticalPixels) * unitsPerPixel;
    m_camera->setPosition(position + translation);
    const Ogre::Vector3 movedTarget = target + translation;
    m_cameraTarget = {movedTarget.x, movedTarget.y, movedTarget.z};
    return true;
}

bool OgreViewportSurface::zoomCamera(double wheelSteps)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    if (m_globeMode)
        return m_globeCameraValid && globeZoom(wheelSteps);
    const Ogre::Real factor = static_cast<Ogre::Real>(std::pow(0.85, wheelSteps));
    if (m_camera->getProjectionType() == Ogre::ProjectionType::PT_ORTHOGRAPHIC) {
        m_camera->setOrthoWindowHeight(qBound<Ogre::Real>(
            0.001f, m_camera->getOrthoWindowHeight() * factor, 1.0e9f));
        return true;
    }
    const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                               static_cast<Ogre::Real>(m_cameraTarget[1]),
                               static_cast<Ogre::Real>(m_cameraTarget[2]));
    Ogre::Vector3 offset = m_camera->getDerivedPosition() - target;
    const Ogre::Real distance = qBound<Ogre::Real>(0.05f, offset.length() * factor, 1.0e9f);
    if (offset.squaredLength() < 0.000001f) offset = Ogre::Vector3::UNIT_Z;
    m_camera->setPosition(target + offset.normalisedCopy() * distance);
    updateClipDistances(distance);
    return true;
}

bool OgreViewportSurface::viewportPointOnTargetPlane(
    double xPixels, double yPixels, std::array<double, 3>* point) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || !point || width() <= 0 || height() <= 0 ||
        !std::isfinite(xPixels) || !std::isfinite(yPixels)) return false;
    const Ogre::Real x = static_cast<Ogre::Real>(xPixels / width());
    const Ogre::Real y = static_cast<Ogre::Real>(yPixels / height());
    const Ogre::Ray ray = m_camera->getCameraToViewportRay(x, y);
    const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                               static_cast<Ogre::Real>(m_cameraTarget[1]),
                               static_cast<Ogre::Real>(m_cameraTarget[2]));
    Ogre::Vector3 normal = m_camera->getDerivedDirection();
    if (normal.squaredLength() < 0.000001f) return false;
    normal.normalise();
    const Ogre::Real denominator = ray.getDirection().dotProduct(normal);
    if (qAbs(denominator) < 0.000001f) return false;
    const Ogre::Real distance = (target - ray.getOrigin()).dotProduct(normal) / denominator;
    if (!std::isfinite(distance) || distance <= 0.0f) return false;
    const Ogre::Vector3 result = ray.getPoint(distance);
    *point = {result.x, result.y, result.z};
    return true;
}

// 光标下的真实模型表面点。Cesium 的相机控制器同样用拾取点做缩放/轨道枢轴；
// 用"目标平面"交点会把枢轴留在数据集中心深度，放大时相机一路钻进数据体。
bool OgreViewportSurface::pickScenePointAt(double xPixels, double yPixels,
                                           std::array<double, 3>* point) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || !point || width() <= 0 || height() <= 0 ||
        !std::isfinite(xPixels) || !std::isfinite(yPixels)) return false;
    const Ogre::Ray ray = m_camera->getCameraToViewportRay(
        static_cast<Ogre::Real>(xPixels / width()),
        static_cast<Ogre::Real>(yPixels / height()));
    const Ogre::Vector3 origin = ray.getOrigin();
    const Ogre::Vector3 direction = ray.getDirection();
    ViewportPickResult pick;
    if (!m_scene.pickRay({origin.x, origin.y, origin.z},
                         {direction.x, direction.y, direction.z}, &pick) ||
        !pick.hasWorldPosition)
        return false;
    *point = pick.worldPosition;
    return true;
}

bool OgreViewportSurface::setNavigationPivotAt(double xPixels, double yPixels,
                                               bool fallbackToView)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || width() <= 0 || height() <= 0 ||
        !std::isfinite(xPixels) || !std::isfinite(yPixels)) return false;
    if (m_globeMode) {
        if (!m_globeCameraValid) return false;
        std::array<double, 3> point;
        std::array<double, 3> anchor;
        if (pickScenePointAt(xPixels, yPixels, &point) &&
            m_scene.globeRenderOrigin(&anchor)) {
            m_globeTargetEcef = GlobeMath::add(point, anchor);
        } else {
            std::array<double, 3> ecef;
            if (globeSurfacePointAt(xPixels, yPixels, &ecef)) {
                m_globeTargetEcef = ecef;
            } else {
                if (!fallbackToView) return false;
                const Ogre::Ray ray = m_camera->getCameraToViewportRay(
                    static_cast<Ogre::Real>(xPixels / width()),
                    static_cast<Ogre::Real>(yPixels / height()));
                const Ogre::Vector3 forward = ray.getDirection();
                const double distance = GlobeMath::length(
                    GlobeMath::sub(m_globePositionEcef, m_globeTargetEcef));
                if (forward.squaredLength() < 0.000001f || distance < 0.001) return false;
                m_globeTargetEcef = GlobeMath::add(
                    m_globePositionEcef,
                    GlobeMath::scale({forward.x, forward.y, forward.z}, distance));
            }
        }
        return applyGlobeCamera();
    }
    std::array<double, 3> point;
    if (!pickScenePointAt(xPixels, yPixels, &point)) {
        if (!fallbackToView) return false;
        // 天空没有表面点：取点击方向、与旧枢轴同距离的点。否则会继续绕
        // 上一次点击留下的偏屏枢轴转，第一格就甩一下。
        const Ogre::Vector3 target(static_cast<Ogre::Real>(m_cameraTarget[0]),
                                   static_cast<Ogre::Real>(m_cameraTarget[1]),
                                   static_cast<Ogre::Real>(m_cameraTarget[2]));
        const Ogre::Vector3 position = m_camera->getDerivedPosition();
        const Ogre::Ray ray = m_camera->getCameraToViewportRay(
            static_cast<Ogre::Real>(xPixels / width()),
            static_cast<Ogre::Real>(yPixels / height()));
        Ogre::Vector3 forward = ray.getDirection();
        const Ogre::Real distance = (position - target).length();
        if (forward.squaredLength() < 0.000001f || distance < 0.001f) return false;
        forward.normalise();
        const Ogre::Vector3 pivot = position + forward * distance;
        point = {pivot.x, pivot.y, pivot.z};
    }
    m_cameraTarget = point;
    updateClipDistances((m_camera->getDerivedPosition() -
                         Ogre::Vector3(static_cast<Ogre::Real>(point[0]),
                                       static_cast<Ogre::Real>(point[1]),
                                       static_cast<Ogre::Real>(point[2]))).length());
    return true;
}

bool OgreViewportSurface::setNavigationTargetFromViewportPoint(double xPixels, double yPixels)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::array<double, 3> point;
    if (!viewportPointOnTargetPlane(xPixels, yPixels, &point)) return false;
    m_cameraTarget = point;
    return true;
}

bool OgreViewportSurface::zoomCameraAt(double xPixels, double yPixels, double wheelSteps)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera) return false;
    if (m_globeMode) {
        if (!m_globeCameraValid) return false;
        // 光标下的瓦片表面优先；点在天空时退回椭球交点，再不行沿用旧枢轴。
        std::array<double, 3> point;
        std::array<double, 3> anchor;
        if (pickScenePointAt(xPixels, yPixels, &point) &&
            m_scene.globeRenderOrigin(&anchor)) {
            m_globeTargetEcef = GlobeMath::add(point, anchor);
        } else {
            std::array<double, 3> ecef;
            if (globeSurfacePointAt(xPixels, yPixels, &ecef))
                m_globeTargetEcef = ecef;
        }
        return globeZoom(wheelSteps);
    }
    if (m_camera->getProjectionType() != Ogre::ProjectionType::PT_ORTHOGRAPHIC) {
        // 有表面点就以它为枢轴推拉（光标下的细节保持不动）；没有就沿当前枢轴缩放。
        setNavigationPivotAt(xPixels, yPixels, false);
        return zoomCamera(wheelSteps);
    }
    std::array<double, 3> before;
    if (!viewportPointOnTargetPlane(xPixels, yPixels, &before))
        return zoomCamera(wheelSteps);
    if (!zoomCamera(wheelSteps)) return false;
    std::array<double, 3> after;
    if (!viewportPointOnTargetPlane(xPixels, yPixels, &after)) return true;
    const Ogre::Vector3 translation(
        static_cast<Ogre::Real>(before[0] - after[0]),
        static_cast<Ogre::Real>(before[1] - after[1]),
        static_cast<Ogre::Real>(before[2] - after[2]));
    m_camera->setPosition(m_camera->getDerivedPosition() + translation);
    m_cameraTarget = {m_cameraTarget[0] + translation.x,
                      m_cameraTarget[1] + translation.y,
                      m_cameraTarget[2] + translation.z};
    return true;
}


bool OgreViewportSurface::setNavigationSettings(const NavigationSettings& settings)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!std::isfinite(settings.orbitDegreesPerPixel) ||
        !std::isfinite(settings.panSensitivity) ||
        !std::isfinite(settings.zoomSensitivity) ||
        !std::isfinite(settings.keyboardMoveSpeed) ||
        settings.orbitDegreesPerPixel < 0.01 || settings.orbitDegreesPerPixel > 5.0 ||
        settings.panSensitivity < 0.05 || settings.panSensitivity > 20.0 ||
        settings.zoomSensitivity < 0.05 || settings.zoomSensitivity > 10.0)
        return false;
    if (settings.keyboardMoveSpeed < 0.05 || settings.keyboardMoveSpeed > 20.0)
        return false;
    m_navigationSettings = settings;
    return true;
}


bool OgreViewportSurface::moveCameraLocal(double rightUnits, double upUnits,
                                         double forwardUnits)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || !std::isfinite(rightUnits) || !std::isfinite(upUnits) ||
        !std::isfinite(forwardUnits)) return false;
    if (m_globeMode)
        return m_globeCameraValid && globeMoveLocal(rightUnits, upUnits, forwardUnits);
    Ogre::Vector3 translation =
        m_camera->getDerivedRight() * static_cast<Ogre::Real>(rightUnits) +
        m_camera->getDerivedUp() * static_cast<Ogre::Real>(upUnits) +
        m_camera->getDerivedDirection() * static_cast<Ogre::Real>(forwardUnits);
    if (m_planarMode)
        // 正俯视时视线就是 -Y：接受屏幕左右/上下平移，但抹掉竖直分量，
        // 相机不会钻进地面，观察平面高度保持不变。
        translation.y = 0.0f;
    if (translation.squaredLength() < 0.0000001f) return true;
    m_camera->setPosition(m_camera->getDerivedPosition() + translation);
    m_cameraTarget = {m_cameraTarget[0] + translation.x,
                      m_cameraTarget[1] + translation.y,
                      m_cameraTarget[2] + translation.z};
    return true;
}

bool OgreViewportSurface::moveCameraContinuously(double rightAxis, double upAxis,
                                                double forwardAxis, double elapsedSeconds,
                                                bool accelerated)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_camera || !std::isfinite(rightAxis) || !std::isfinite(upAxis) ||
        !std::isfinite(forwardAxis) || !std::isfinite(elapsedSeconds) ||
        elapsedSeconds < 0.0 || elapsedSeconds > 0.25 || qAbs(rightAxis) > 1.0 ||
        qAbs(upAxis) > 1.0 || qAbs(forwardAxis) > 1.0) return false;
    const double magnitude = std::sqrt(rightAxis * rightAxis + upAxis * upAxis +
                                       forwardAxis * forwardAxis);
    if (magnitude < 0.000001 || elapsedSeconds == 0.0) return true;
    ViewportCameraState state;
    if (!cameraState(&state)) return false;
    const double dx = state.position[0] - state.target[0];
    const double dy = state.position[1] - state.target[1];
    const double dz = state.position[2] - state.target[2];
    const double sceneScale = qMax(0.1, std::sqrt(dx * dx + dy * dy + dz * dz));
    double units = sceneScale * 0.75 * m_navigationSettings.keyboardMoveSpeed * elapsedSeconds;
    if (accelerated) units *= 4.0;
    const double normalization = magnitude > 1.0 ? 1.0 / magnitude : 1.0;
    return moveCameraLocal(rightAxis * normalization * units,
                           upAxis * normalization * units,
                           forwardAxis * normalization * units);
}



void OgreViewportSurface::resizeEvent(QResizeEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    QWidget::resizeEvent(event);
    resizeRenderWindow();
}

void OgreViewportSurface::mousePressEvent(QMouseEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if ((event->button() == Qt::MiddleButton) ||
        (event->button() == Qt::RightButton)) {
        // 按在模型上时以该表面点为平移/旋转枢轴（Cesium 的拾取枢轴）；
        // 按在天空上则取视线前方同距离的点，避免沿用偏屏的旧枢轴。
        setNavigationPivotAt(event->position().x(), event->position().y(), true);
        m_navigationButton = event->button();
        m_navigationLastPosition = event->position().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton && m_rectSelectCallback) {
        m_rectSelectStart = event->position().toPoint();
        m_rectSelectActive = true;
    }
    if (event->button() != Qt::LeftButton || !m_camera || m_pickListeners.empty() ||
        width() <= 0 || height() <= 0) return;
    dispatchPick(pickAtPosition(event->position()));
}

ViewportPickResult OgreViewportSurface::pickAtPosition(const QPointF& position) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ViewportPickResult result;
    if (!m_camera || width() <= 0 || height() <= 0)
        return result;
    const Ogre::Ray ray = m_camera->getCameraToViewportRay(
        static_cast<Ogre::Real>(position.x() / width()),
        static_cast<Ogre::Real>(position.y() / height()));
    const Ogre::Vector3 origin = ray.getOrigin();
    const Ogre::Vector3 direction = ray.getDirection();
    m_scene.pickRay({origin.x, origin.y, origin.z},
                    {direction.x, direction.y, direction.z}, &result);
    return result;
}

bool OgreViewportSurface::fillGroundPosition(const QPointF& position,
                                             ViewportPickResult* result) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!result || !m_camera || width() <= 0 || height() <= 0)
        return false;
    const Ogre::Ray ray = m_camera->getCameraToViewportRay(
        static_cast<Ogre::Real>(position.x() / width()),
        static_cast<Ogre::Real>(position.y() / height()));
    const Ogre::Vector3 origin = ray.getOrigin();
    const Ogre::Vector3 direction = ray.getDirection();
    if (m_globeMode) {
        // 球面没有 y=0 地面：悬停天空时给视线与椭球的交点（渲染空间坐标，
        // 读数端加回锚点转经纬度）。
        if (!m_globeCameraValid)
            return false;
        std::array<double, 3> ecef;
        std::array<double, 3> anchor;
        if (!globeSurfacePointAt(position.x(), position.y(), &ecef) ||
            !m_scene.globeRenderOrigin(&anchor))
            return false;
        result->worldPosition = GlobeMath::sub(ecef, anchor);
        result->hasWorldPosition = true;
        return true;
    }
    // 视线朝上或与地面近平行时没有交点，交给调用方按「没有坐标」处理。
    if (std::abs(direction.y) < 1e-6)
        return false;
    const double t = -static_cast<double>(origin.y) / static_cast<double>(direction.y);
    if (!std::isfinite(t) || t <= 0.0)
        return false;
    result->worldPosition = {origin.x + direction.x * t, 0.0,
                             origin.z + direction.z * t};
    result->hasWorldPosition = true;
    return true;
}

void OgreViewportSurface::leaveEvent(QEvent* event)
{
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        if (!m_hoverListeners.empty())
            dispatchHover(ViewportPickResult {});   // 光标离开：读数没有值，别留着上一次的
    }
    QWidget::leaveEvent(event);
}

void OgreViewportSurface::mouseMoveEvent(QMouseEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_rectSelectActive && (event->buttons() & Qt::LeftButton)) {
        m_rectSelectCurrent = event->position().toPoint();
        event->accept();
        return;
    }
    if (m_navigationButton == Qt::NoButton) {
        QWidget::mouseMoveEvent(event);
        if (!m_rectSelectActive && !m_hoverListeners.empty() && m_camera) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastHoverPickMs >= 80) {
                m_lastHoverPickMs = now;
                ViewportPickResult result = pickAtPosition(event->position());
                // 没碰到模型也发一条：底部读数要么给地面交点，要么明确没有值。
                if (!result.hasWorldPosition)
                    fillGroundPosition(event->position(), &result);
                dispatchHover(result);
            }
        }
        return;
    }
    const QPoint current = event->position().toPoint();
    const QPoint delta = current - m_navigationLastPosition;
    m_navigationLastPosition = current;
    if (m_navigationButton == Qt::MiddleButton)
        panCamera(delta.x() * m_navigationSettings.panSensitivity,
                  delta.y() * m_navigationSettings.panSensitivity);
    else if (m_navigationButton == Qt::RightButton) {
        if (m_planarMode) {
            // 二维不接受旋转：右键拖拽改成平移，保持"拖哪跟哪"的手感。
            panCamera(delta.x() * m_navigationSettings.panSensitivity,
                      delta.y() * m_navigationSettings.panSensitivity);
            event->accept();
            return;
        }
        // 水平方向用的是「抓住地球仪」约定：鼠标往右拖，相机往左绕，内容跟着光标走。
        // 竖直方向必须用同一个约定（Qt 屏幕坐标 Y 轴朝下，所以默认取 +1 而不是 -1），
        // 否则横竖两套约定，看起来就是上下反了。
        const double horizontalSign = m_navigationSettings.invertOrbitHorizontal ? 1.0 : -1.0;
        const double verticalSign = m_navigationSettings.invertOrbitVertical ? -1.0 : 1.0;
        orbitCamera(delta.x() * m_navigationSettings.orbitDegreesPerPixel * horizontalSign,
                    delta.y() * m_navigationSettings.orbitDegreesPerPixel * verticalSign);
    }
    event->accept();
}

void OgreViewportSurface::mouseReleaseEvent(QMouseEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (event->button() == Qt::LeftButton && m_rectSelectActive) {
        m_rectSelectActive = false;
        const QPoint current = event->position().toPoint();
        if ((current - m_rectSelectStart).manhattanLength() >= 5 && m_camera &&
            m_rectSelectCallback) {
            const int left = qMin(m_rectSelectStart.x(), current.x());
            const int right = qMax(m_rectSelectStart.x(), current.x());
            const int top = qMin(m_rectSelectStart.y(), current.y());
            const int bottom = qMax(m_rectSelectStart.y(), current.y());
            QList<QString> hitLayers;
            const Ogre::Real w = static_cast<Ogre::Real>(qMax(1, width()));
            const Ogre::Real h = static_cast<Ogre::Real>(qMax(1, height()));
            const QRectF selectRect(left, top, right - left, bottom - top);
            const auto projectWorld = [&](const Ogre::Vector3& world) -> QPointF {
                const Ogre::Vector4 projected =
                    m_camera->getProjectionMatrix() * m_camera->getViewMatrix() *
                    Ogre::Vector4(world, 1.0f);
                if (projected.w == 0.0f)
                    return {-1.0e9, -1.0e9};
                const Ogre::Real ndcX = projected.x / projected.w;
                const Ogre::Real ndcY = projected.y / projected.w;
                return {(ndcX * 0.5f + 0.5f) * w,
                        (0.5f - ndcY * 0.5f) * h};
            };
            const auto pointInTriangle = [](const QPointF& p, const QPointF& a,
                                            const QPointF& b, const QPointF& c) {
                const auto sign = [](const QPointF& p1, const QPointF& p2,
                                     const QPointF& p3) {
                    return (p1.x() - p3.x()) * (p2.y() - p3.y()) -
                           (p2.x() - p3.x()) * (p1.y() - p3.y());
                };
                const double d1 = sign(p, a, b);
                const double d2 = sign(p, b, c);
                const double d3 = sign(p, c, a);
                const bool hasNegative = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
                const bool hasPositive = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
                return !(hasNegative && hasPositive);
            };
            const auto segmentsIntersect = [](const QPointF& p1, const QPointF& p2,
                                              const QPointF& q1, const QPointF& q2) {
                const auto orientation = [](const QPointF& a, const QPointF& b,
                                            const QPointF& c) {
                    const double value = (b.y() - a.y()) * (c.x() - b.x()) -
                                         (b.x() - a.x()) * (c.y() - b.y());
                    return (value > 1.0e-9) - (value < -1.0e-9);
                };
                const int o1 = orientation(p1, p2, q1);
                const int o2 = orientation(p1, p2, q2);
                const int o3 = orientation(q1, q2, p1);
                const int o4 = orientation(q1, q2, p2);
                return o1 != o2 && o3 != o4;
            };
            const auto triangleIntersectsRect =
                [&](const QPointF& a, const QPointF& b, const QPointF& c) {
                    if (selectRect.contains(a) || selectRect.contains(b) ||
                        selectRect.contains(c))
                        return true;
                    const QPointF corners[4] = {
                        selectRect.topLeft(), selectRect.topRight(),
                        selectRect.bottomRight(), selectRect.bottomLeft()};
                    for (const QPointF& corner : corners)
                        if (pointInTriangle(corner, a, b, c))
                            return true;
                    const QPointF triangleEdges[3][2] = {{a, b}, {b, c}, {c, a}};
                    const QPointF rectEdges[4][2] = {
                        {corners[0], corners[1]}, {corners[1], corners[2]},
                        {corners[2], corners[3]}, {corners[3], corners[0]}};
                    for (const auto& edge : triangleEdges)
                        for (const auto& rectEdge : rectEdges)
                            if (segmentsIntersect(edge[0], edge[1],
                                                  rectEdge[0], rectEdge[1]))
                                return true;
                    return false;
                };
            const bool containMode =
                (event->modifiers() & Qt::ControlModifier) != 0;
            const QList<QString> layerKeys = m_scene.modelLayerKeys();
            for (const QString& layerKey : layerKeys) {
                if (!m_scene.isModelLayerVisible(layerKey))
                    continue;
                RenderBounds bounds;
                if (!m_scene.modelLayerBounds(layerKey, &bounds) || !bounds.valid)
                    continue;
                const Ogre::Vector3 corners[8] = {
                    {static_cast<Ogre::Real>(bounds.minimum[0]),
                     static_cast<Ogre::Real>(bounds.minimum[1]),
                     static_cast<Ogre::Real>(bounds.minimum[2])},
                    {static_cast<Ogre::Real>(bounds.maximum[0]),
                     static_cast<Ogre::Real>(bounds.minimum[1]),
                     static_cast<Ogre::Real>(bounds.minimum[2])},
                    {static_cast<Ogre::Real>(bounds.minimum[0]),
                     static_cast<Ogre::Real>(bounds.maximum[1]),
                     static_cast<Ogre::Real>(bounds.minimum[2])},
                    {static_cast<Ogre::Real>(bounds.minimum[0]),
                     static_cast<Ogre::Real>(bounds.minimum[1]),
                     static_cast<Ogre::Real>(bounds.maximum[2])},
                    {static_cast<Ogre::Real>(bounds.maximum[0]),
                     static_cast<Ogre::Real>(bounds.maximum[1]),
                     static_cast<Ogre::Real>(bounds.minimum[2])},
                    {static_cast<Ogre::Real>(bounds.maximum[0]),
                     static_cast<Ogre::Real>(bounds.minimum[1]),
                     static_cast<Ogre::Real>(bounds.maximum[2])},
                    {static_cast<Ogre::Real>(bounds.minimum[0]),
                     static_cast<Ogre::Real>(bounds.maximum[1]),
                     static_cast<Ogre::Real>(bounds.maximum[2])},
                    {static_cast<Ogre::Real>(bounds.maximum[0]),
                     static_cast<Ogre::Real>(bounds.maximum[1]),
                     static_cast<Ogre::Real>(bounds.maximum[2])}};
                QRectF projectedBounds;
                bool anyProjected = false;
                bool allInside = true;
                for (const Ogre::Vector3& corner : corners) {
                    const QPointF screen = projectWorld(corner);
                    if (screen.x() < -1.0e8)
                        continue;
                    anyProjected = true;
                    const QRectF pointRect(screen, QSizeF(0.0, 0.0));
                    projectedBounds = projectedBounds.isNull()
                                           ? pointRect
                                           : projectedBounds.united(pointRect);
                    if (!selectRect.contains(screen))
                        allInside = false;
                }
                if (!anyProjected)
                    continue;
                if (containMode) {
                    if (allInside)
                        hitLayers.append(layerKey);
                    continue;
                }
                if (!projectedBounds.intersects(selectRect))
                    continue;
                const bool tilesetLayer =
                    layerKey.startsWith(QStringLiteral("tile:"));
                const auto mesh = m_scene.modelLayerMesh(layerKey);
                if (tilesetLayer || !mesh) {
                    hitLayers.append(layerKey);
                    continue;
                }
                std::array<double, 3> offset {};
                m_scene.modelLayerOffset(layerKey, &offset);
                bool hit = false;
                for (const ModelPrimitiveDto& primitive : mesh->primitives) {
                    const std::uint32_t end =
                        primitive.firstIndex + primitive.indexCount;
                    for (std::uint32_t index = primitive.firstIndex;
                         index + 2 < end; index += 3) {
                        const std::uint32_t i0 = mesh->indices[index];
                        const std::uint32_t i1 = mesh->indices[index + 1];
                        const std::uint32_t i2 = mesh->indices[index + 2];
                        if (i0 >= mesh->vertices.size() ||
                            i1 >= mesh->vertices.size() ||
                            i2 >= mesh->vertices.size())
                            continue;
                        const MeshVertexDto& v0 = mesh->vertices[i0];
                        const MeshVertexDto& v1 = mesh->vertices[i1];
                        const MeshVertexDto& v2 = mesh->vertices[i2];
                        const QPointF a = projectWorld(
                            Ogre::Vector3(v0.x + offset[0], v0.y + offset[1],
                                          v0.z + offset[2]));
                        const QPointF b = projectWorld(
                            Ogre::Vector3(v1.x + offset[0], v1.y + offset[1],
                                          v1.z + offset[2]));
                        const QPointF c = projectWorld(
                            Ogre::Vector3(v2.x + offset[0], v2.y + offset[1],
                                          v2.z + offset[2]));
                        if (triangleIntersectsRect(a, b, c)) {
                            hit = true;
                            break;
                        }
                    }
                    if (hit)
                        break;
                }
                if (hit)
                    hitLayers.append(layerKey);
            }
            const bool additive = event->modifiers() & Qt::ShiftModifier;
            m_rectSelectCallback(hitLayers, additive);
        }
        event->accept();
        return;
    }
    if (event->button() == m_navigationButton) {
        m_navigationButton = Qt::NoButton;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void OgreViewportSurface::wheelEvent(QWheelEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const QPoint angle = event->angleDelta();
    if (!angle.isNull() &&
        zoomCameraAt(event->position().x(), event->position().y(),
                     angle.y() / 120.0 * m_navigationSettings.zoomSensitivity)) {
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void OgreViewportSurface::keyPressEvent(QKeyEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (event->key() == Qt::Key_Escape && !m_pickListeners.empty()) {
        dispatchPick({});
        event->accept();
        return;
    }
    switch (event->key()) {
    case Qt::Key_W: case Qt::Key_S: case Qt::Key_A: case Qt::Key_D:
    case Qt::Key_Q: case Qt::Key_E: case Qt::Key_Shift:
        m_navigationKeys.insert(event->key());
        event->accept();
        return;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

void OgreViewportSurface::keyReleaseEvent(QKeyEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_navigationKeys.contains(event->key())) {
        m_navigationKeys.remove(event->key());
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void OgreViewportSurface::focusOutEvent(QFocusEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_navigationKeys.clear();
    QWidget::focusOutEvent(event);
}

QPaintEngine* OgreViewportSurface::paintEngine() const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return nullptr;
}

bool OgreViewportSurface::event(QEvent* event)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const bool handled = QWidget::event(event);
    const QEvent::Type type = event->type();
    if (type == QEvent::Show) {
        std::cerr << "VIEW3D_OGRE_STAGE: show_event\n";
    }
    if (type == QEvent::DevicePixelRatioChange || type == QEvent::Show) {
        resizeRenderWindow();
    }
    if (type == QEvent::Show) {
        // 首次启动时停靠布局/最大化可能在本事件之后才稳定，再补一次 UI 线程同步。
        QTimer::singleShot(0, this, [this] { refreshRenderResolution(); });
    }
    if (type == QEvent::Show || type == QEvent::Hide ||
        type == QEvent::WindowStateChange) {
        updateRenderActive();
    }
    return handled;
}

QSize OgreViewportSurface::renderPixelSize() const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    // 返回刚请求的目标分辨率；实际窗口在渲染线程下一帧才切换。
    if (m_requestedRenderWidth > 0u && m_requestedRenderHeight > 0u) {
        return QSize(static_cast<int>(m_requestedRenderWidth),
                     static_cast<int>(m_requestedRenderHeight));
    }
    if (!m_window)
        return {};
    return QSize(static_cast<int>(m_window->getWidth()),
                 static_cast<int>(m_window->getHeight()));
}

void OgreViewportSurface::resizeRenderWindow()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_window || width() <= 0 || height() <= 0) {
        return;
    }
    if (!m_resizeLogged) {
        m_resizeLogged = true;
        std::cerr << "VIEW3D_OGRE_STAGE: first_resize_begin "
                  << width() << "x" << height() << "\n";
    }
    const qreal scale = devicePixelRatioF();
    const Ogre::uint32 targetWidth =
        static_cast<Ogre::uint32>(qMax(1, qRound(width() * scale)));
    const Ogre::uint32 targetHeight =
        static_cast<Ogre::uint32>(qMax(1, qRound(height() * scale)));
    m_requestedRenderWidth = targetWidth;
    m_requestedRenderHeight = targetHeight;
    if (m_window->getWidth() != targetWidth || m_window->getHeight() != targetHeight) {
        std::cerr << "VIEW3D_RENDER_RESOLUTION view=" << m_instanceSuffix
                  << " widget=" << width() << "x" << height() << " dpr=" << scale
                  << " from=" << m_window->getWidth() << "x" << m_window->getHeight()
                  << " to=" << targetWidth << "x" << targetHeight << std::endl;
    }
    m_window->requestResolution(targetWidth, targetHeight);
    m_window->windowMovedOrResized();
    if (false)
        std::cerr << "VIEW3D_OGRE_STAGE: first_resize_done\n";
}

} // namespace View3D
