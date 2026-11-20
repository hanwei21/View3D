#include "ViewExtension.h"
#include <IApplication.h>
#include <IUiObjectRegistry.h>
#include <IPluginContainer.h>
#include <IPluginRegistryService.h>
#include <View3DObjectIds.h>
#include <IRenderSurface.h>
#include <RenderTypes.h>
#include "SystemConfig.h"
#include <QApplication>
#include <QBuffer>
#include <iostream>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSlider>
#include <QSettings>
#include <QSaveFile>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <cmath>
namespace View3DPlugins {
namespace {
QString systemConfigPath() {
    const QString applicationDir = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {QDir(applicationDir).filePath(QStringLiteral("system_config.xml")),
                                     QDir(applicationDir).filePath(QStringLiteral("config/system_config.xml"))}) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QDir(applicationDir).filePath(QStringLiteral("system_config.xml"));
}
// 与主题/Ribbon 布局同一个文件：用户在界面上改过的东西记在这里，
// 优先于 system_config.xml 的运行默认值。
QString layoutStatePath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("layout_state.ini"));
}
} // namespace

void ViewExtension::applyToViews(
    const std::function<void(View3D::IRenderSurface *)> &action) {
    if (!m_layout || !action)
        return;
    for (View3D::IRenderSurface *view : m_layout->views())
        action(view);
}

View3D::IRenderSurface *ViewExtension::currentView() const {
    if (m_layout) {
        if (auto *view = m_layout->activeView())
            return view;
    }
    return m_viewport;
}

bool ViewExtension::configure(RF::PluginEngine::IApplication *r) {
    m_application = r;
    m_window = r ? r->getMainWindow() : nullptr;
    return m_window;
}
bool ViewExtension::start(RF::PluginEngine::IApplication *r) {
    m_application = r;
    auto *ui = r->getUiObjectRegistry();
    auto *objects = r->getPluginContainer();
    m_layout = objects
        ? objects->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!m_layout || !m_layout->initialize())
        return false;
    m_viewport = m_layout->primaryView();
    if (!currentView())
        return false;
    m_navigationHelp =
        qobject_cast<QLabel *>(ui->widget(QStringLiteral("overlay.navigation_help")));
    m_cameraStatus = qobject_cast<QLabel *>(ui->widget(QStringLiteral("status.camera")));

    QString configWarning;
    const View3D::SystemConfig config =
        View3D::SystemConfig::load(systemConfigPath(), &configWarning);
    if (!configWarning.isEmpty())
        std::cerr << configWarning.toStdString() << '\n';

    m_layout->applySettings(config.viewport);
    applyStoredViewMode();
    m_tileCache = config.tileCache;
    std::cerr << "VIEW3D_TILECACHE preset=" << m_tileCache.preset.toStdString()
              << " cache_mb=" << (m_tileCache.cacheBytes >> 20)
              << " overflow_mb=" << (m_tileCache.overflowBytes >> 20)
              << " index_mb=" << (m_tileCache.indexCacheBytes >> 20)
              << " max_tiles=" << m_tileCache.maxTiles
              << " sse=" << m_tileCache.screenSpaceErrorPixels
              << " workers=" << m_tileCache.workers
              << " inflight=" << m_tileCache.maxInFlight
              << " keepalive_ms=" << m_tileCache.keepAliveMs << std::endl;
    objects->publishAs<View3D::ITileCacheConfig>(View3D::ServiceIds::TileCache, this);

    objects->publishAs<View3D::IViewportCamera>(View3D::ServiceIds::Viewport, m_viewport);
    publishStates();
    objects->publishAs<RF::PluginEngine::IViewCommandService>(
        View3D::ServiceIds::ViewCommands, this);
    return true;
}
void ViewExtension::stop() {
    if (m_application)
        m_application->getPluginContainer()->unpublishObject(
            View3D::ServiceIds::TileCache,
            static_cast<View3D::ITileCacheConfig *>(this));
    if (m_application)
        m_application->getPluginContainer()->unpublishObject(
            View3D::ServiceIds::ViewCommands,
            static_cast<RF::PluginEngine::IViewCommandService *>(this));
    if (m_application && m_viewport)
        m_application->getPluginContainer()->unpublishObject(
            View3D::ServiceIds::Viewport,
            static_cast<View3D::IViewportCamera *>(m_viewport));
    m_layout = nullptr;
    m_viewport = nullptr;
    m_window = nullptr;
    m_application = nullptr;
}
bool ViewExtension::commandEnabled(const QString &id) const {
    if (!planarModeActive() && !globeModeActive())
        return true;
    // 二维锁姿态、球面只接受自由轨道：立面视角与投影切换都无意义。
    static const QStringList disabled = {QStringLiteral("cmd.view.front"),
                                         QStringLiteral("cmd.view.back"),
                                         QStringLiteral("cmd.view.left"),
                                         QStringLiteral("cmd.view.right"),
                                         QStringLiteral("cmd.view.bottom"),
                                         QStringLiteral("cmd.view.isometric"),
                                         QStringLiteral("cmd.projection.perspective"),
                                         QStringLiteral("cmd.projection.orthographic")};
    if (disabled.contains(id))
        return false;
    // 球面没有平面地面网格/坐标轴：这两个开关没有视觉反馈，直接禁用。
    if (globeModeActive() &&
        (id == QStringLiteral("cmd.environment.grid") ||
         id == QStringLiteral("cmd.environment.axes")))
        return false;
    return true;
}
bool ViewExtension::planarModeActive() const {
    // 布局是模式的唯一口径：它按主视口的真实状态聚合，不会因为
    // 某个视口没进模式而出现"界面说二维、画面是三维"。
    if (m_layout)
        return m_layout->planarMode();
    const auto* view = currentView();
    return view && view->planarMode();
}
bool ViewExtension::globeModeActive() const {
    // 球面模式挂在场景上，布局聚合后再往外报。
    if (m_layout)
        return m_layout->globeModeEnabled();
    const auto* view = currentView();
    return view && view->globeModeEnabled();
}
bool ViewExtension::isFullscreen() const {
    return m_window && m_window->isFullScreen();
}
void ViewExtension::setFullscreen(bool enabled) {
    if (!m_window)
        return;
    if (enabled)
        m_window->showFullScreen();
    else
        m_window->showNormal();
}

void ViewExtension::setQualityPreset(int preset) {
    if (!currentView())
        return;
    if (preset == 0) {
        setShadowsEnabled(false);
        setShadowQuality(View3D::ViewportShadowQuality::Performance);
        currentView()->setGridVisible(false);
        currentView()->setAxesVisible(false);
    } else if (preset == 1) {
        setShadowsEnabled(true);
        setShadowQuality(View3D::ViewportShadowQuality::Balanced);
        currentView()->setGridVisible(true);
        currentView()->setAxesVisible(true);
    } else {
        setShadowsEnabled(true);
        setShadowQuality(View3D::ViewportShadowQuality::Quality);
        currentView()->setGridVisible(true);
        currentView()->setAxesVisible(true);
    }
}

bool ViewExtension::commandChecked(const QString &id) const {
    if (id == QStringLiteral("cmd.view.fullscreen"))
        return isFullscreen();
    if (!currentView())
        return false;
    if (id == QStringLiteral("cmd.environment.grid"))
        return currentView()->gridVisible();
    if (id == QStringLiteral("cmd.environment.axes"))
        return currentView()->axesVisible();
    if (id == QStringLiteral("cmd.environment.ambient_light"))
        return currentView()->lightingStyle().ambientVisible;
    if (id == QStringLiteral("cmd.environment.main_light"))
        return currentView()->lightingStyle().mainLightVisible;
    if (id == QStringLiteral("cmd.environment.shadows"))
        return currentView()->shadowStyle().enabled;
    if (id == QStringLiteral("cmd.display.solid"))
        return currentView()->displayMode() == View3D::ViewportDisplayMode::Solid;
    if (id == QStringLiteral("cmd.display.wireframe"))
        return currentView()->displayMode() == View3D::ViewportDisplayMode::Wireframe;
    if (id == QStringLiteral("cmd.display.solid_wireframe"))
        return currentView()->displayMode() == View3D::ViewportDisplayMode::SolidWireframe;
    if (id == QStringLiteral("cmd.display.points"))
        return currentView()->displayMode() == View3D::ViewportDisplayMode::Points;
    if (id == QStringLiteral("cmd.environment.background_solid"))
        return currentView()->backgroundMode() == View3D::ViewportBackgroundMode::Solid;
    if (id == QStringLiteral("cmd.environment.background_gradient"))
        return currentView()->backgroundMode() == View3D::ViewportBackgroundMode::Gradient;
    if (id == QStringLiteral("cmd.projection.perspective"))
        return currentView()->cameraProjectionMode() == View3D::CameraProjectionMode::Perspective;
    if (id == QStringLiteral("cmd.projection.orthographic"))
        return currentView()->cameraProjectionMode() == View3D::CameraProjectionMode::Orthographic;
    if (id == QStringLiteral("cmd.navigation.help"))
        return m_navigationHelp && m_navigationHelp->isVisible();
    if (id == QStringLiteral("cmd.camera.status"))
        return m_cameraStatus && m_cameraStatus->isVisible();
    if (id == QStringLiteral("cmd.viewport.camera_sync"))
        return m_layout && m_layout->cameraSyncEnabled();
    if (id == QStringLiteral("cmd.view.mode_2d"))
        return planarModeActive();
    if (id == QStringLiteral("cmd.view.mode_globe"))
        return globeModeActive();
    if (id == QStringLiteral("cmd.view.mode_3d"))
        return !planarModeActive() && !globeModeActive();
    return false;
}
void ViewExtension::setStandardView(View3D::StandardCameraView v) {
    if (auto *view = currentView())
        view->setStandardCameraView(v);
}
void ViewExtension::setDisplayMode(View3D::ViewportDisplayMode v) {
    if (auto *view = currentView())
        view->setDisplayMode(v);
}
void ViewExtension::setBackgroundMode(View3D::ViewportBackgroundMode v) {
    applyToViews([v](View3D::IRenderSurface *view) { view->setBackgroundMode(v); });
}
void ViewExtension::setGridVisible(bool v) {
    if (currentView())
        currentView()->setGridVisible(v);
}
void ViewExtension::setAxesVisible(bool v) {
    if (currentView())
        currentView()->setAxesVisible(v);
}
void ViewExtension::setAmbientLightVisible(bool v) {
    if (currentView())
        currentView()->setAmbientLightVisible(v);
}
void ViewExtension::setMainLightVisible(bool v) {
    if (currentView())
        currentView()->setMainLightVisible(v);
}
void ViewExtension::setShadowsEnabled(bool v) {
    applyToViews([v](View3D::IRenderSurface *view) { view->setShadowsEnabled(v); });
}
void ViewExtension::setShadowQuality(View3D::ViewportShadowQuality v) {
    applyToViews([v](View3D::IRenderSurface *view) { view->setShadowQuality(v); });
}
void ViewExtension::setProjectionMode(View3D::CameraProjectionMode v) {
    if (auto *view = currentView())
        view->setCameraProjectionMode(v);
}
void ViewExtension::setPlanarMode(bool enabled) {
    // 模式挂在布局上：所有现有视口一起切换，之后新建的视口也继承当前模式。
    if (m_layout) {
        // 二维与球面互斥：「二维」「三维」都是平面模式，进入前先退球面。
        if (m_layout->globeModeEnabled())
            m_layout->setGlobeModeEnabled(false);
        m_layout->setPlanarMode(enabled);
    }
    // 记住用户的选择：system_config.xml 只是运行默认值，切换过的模式写进布局状态。
    QSettings settings(layoutStatePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("view/mode"),
                      enabled ? QStringLiteral("2d") : QStringLiteral("3d"));
    publishStates();
}

void ViewExtension::setGlobeMode(bool enabled) {
    if (!m_layout)
        return;
    if (enabled && m_layout->planarMode())
        m_layout->setPlanarMode(false);
    const bool applied = m_layout->setGlobeModeEnabled(enabled);
    // 渲染端不支持时不改布局状态：失败的模式不能持久化到下次启动。
    if (enabled && !applied)
        return;
    QSettings settings(layoutStatePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("view/mode"),
                      enabled ? QStringLiteral("globe") : QStringLiteral("3d"));
    publishStates();
}

void ViewExtension::applyStoredViewMode() {
    const QSettings settings(layoutStatePath(), QSettings::IniFormat);
    const QString stored =
        settings.value(QStringLiteral("view/mode")).toString().trimmed().toLower();
    if (stored == QStringLiteral("globe")) {
        setGlobeMode(true);
        return;
    }
    if (stored != QStringLiteral("2d") && stored != QStringLiteral("3d"))
        return;
    setPlanarMode(stored == QStringLiteral("2d"));
}
bool ViewExtension::setViewportLayout(int layout) {
    return m_layout &&
        m_layout->setLayout(static_cast<View3D::ViewportLayout>(layout));
}
bool ViewExtension::setCameraSync(bool enabled) {
    return m_layout && m_layout->setCameraSync(enabled);
}
void ViewExtension::captureViewport() {
    if (!currentView())
        return;
    QString configWarning;
    const View3D::SystemConfig config =
        View3D::SystemConfig::load(systemConfigPath(), &configWarning);
    if (!configWarning.isEmpty())
        std::cerr << configWarning.toStdString() << std::endl;
    const View3D::ViewportCaptureConfig &capture = config.viewportCapture;

    QDialog dialog(m_window);
    dialog.setWindowTitle(QStringLiteral("保存视口截图"));
    auto *layout = new QFormLayout(&dialog);

    auto *format = new QComboBox(&dialog);
    format->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
    format->addItem(QStringLiteral("JPG"), QStringLiteral("jpg"));
    const int formatIndex = format->findData(capture.defaultFormat);
    format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);
    layout->addRow(QStringLiteral("格式"), format);

    auto *quality = new QSlider(Qt::Horizontal, &dialog);
    quality->setRange(1, 100);
    quality->setValue(capture.jpegQuality);
    quality->setEnabled(format->currentData().toString() == QStringLiteral("jpg"));
    layout->addRow(QStringLiteral("JPG 质量"), quality);
    QObject::connect(format, &QComboBox::currentIndexChanged, &dialog,
                     [quality, format] { quality->setEnabled(
                         format->currentData().toString() == QStringLiteral("jpg")); });

    auto *scale = new QDoubleSpinBox(&dialog);
    scale->setRange(0.25, 4.0);
    scale->setDecimals(2);
    scale->setSingleStep(0.25);
    scale->setValue(capture.outputScale);
    scale->setSuffix(QStringLiteral(" 倍"));
    layout->addRow(QStringLiteral("输出倍率"), scale);

    auto *offscreen = new QCheckBox(QStringLiteral("离屏渲染（高分辨率）"), &dialog);
    offscreen->setChecked(false);
    offscreen->setToolTip(QStringLiteral("按目标分辨率真实渲染，不缩放拉伸"));
    layout->addRow(QStringLiteral("渲染方式"), offscreen);
    QObject::connect(offscreen, &QCheckBox::toggled, &dialog,
                     [scale](bool enabled) {
                         scale->setRange(enabled ? 1.0 : 0.25, enabled ? 8.0 : 4.0);
                         if (enabled && scale->value() < 1.0)
                             scale->setValue(1.0);
                     });

    auto *includeGrid = new QCheckBox(QStringLiteral("地面网格"), &dialog);
    includeGrid->setChecked(currentView()->gridVisible());
    auto *includeAxes = new QCheckBox(QStringLiteral("坐标轴"), &dialog);
    includeAxes->setChecked(currentView()->axesVisible());
    auto *includeMeasurements = new QCheckBox(QStringLiteral("测量标记与连线"), &dialog);
    includeMeasurements->setChecked(currentView()->hasOverlay());
    auto *overlayBox = new QWidget(&dialog);
    auto *overlayLayout = new QVBoxLayout(overlayBox);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    overlayLayout->addWidget(includeGrid);
    overlayLayout->addWidget(includeAxes);
    overlayLayout->addWidget(includeMeasurements);
    layout->addRow(QStringLiteral("包含叠加层"), overlayBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool useJpg = format->currentData().toString() == QStringLiteral("jpg");
    const QString filter = useJpg ? QStringLiteral("JPEG (*.jpg)") : QStringLiteral("PNG (*.png)");
    QString p = QFileDialog::getSaveFileName(m_window, QStringLiteral("保存视口截图"), QString(),
                                             filter);
    if (p.isEmpty())
        return;
    if (useJpg && !p.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive))
        p += QStringLiteral(".jpg");
    else if (!useJpg && !p.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        p += QStringLiteral(".png");

    View3D::CaptureOptions options;
    options.includeGrid = includeGrid->isChecked();
    options.includeAxes = includeAxes->isChecked();
    options.includeMeasurementOverlay = includeMeasurements->isChecked();
    const QSize viewportSize = currentView()->widget() ? currentView()->widget()->size() : QSize();
    const QSize outputSize(qMax(1, qRound(viewportSize.width() * scale->value())),
                           qMax(1, qRound(viewportSize.height() * scale->value())));
    const QImage image = offscreen->isChecked()
        ? currentView()->captureImageOffscreen(outputSize, options)
        : currentView()->captureImage(outputSize, options);
    if (image.isNull()) {
        QMessageBox::warning(m_window, QStringLiteral("保存视口截图"),
                             QStringLiteral("无法读取视口画面"));
        return;
    }
    if (useJpg) {
        if (!image.save(p, "JPG", quality->value()))
            QMessageBox::warning(m_window, QStringLiteral("保存视口截图"),
                                 QStringLiteral("无法保存图像"));
    } else if (!image.save(p, "PNG")) {
        QMessageBox::warning(m_window, QStringLiteral("保存视口截图"),
                             QStringLiteral("无法保存图像"));
    }
}
void ViewExtension::showNavigationSettings() {
    if (!currentView())
        return;
    QDialog d(m_window);
    d.setWindowTitle(QStringLiteral("导航设置"));
    auto *l = new QFormLayout(&d);
    auto *orbit = new QDoubleSpinBox(&d);
    orbit->setRange(.01, 5);
    orbit->setValue(currentView()->navigationSettings().orbitDegreesPerPixel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    l->addRow(QStringLiteral("轨道旋转"), orbit);
    l->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted)
        return;
    auto s = currentView()->navigationSettings();
    s.orbitDegreesPerPixel = orbit->value();
    currentView()->setNavigationSettings(s);
}
void ViewExtension::setNavigationHelpVisible(bool v) {
    if (m_navigationHelp)
        m_navigationHelp->setVisible(v);
}
void ViewExtension::setCameraStatusVisible(bool v) {
    if (m_cameraStatus)
        m_cameraStatus->setVisible(v);
}
void ViewExtension::copyCameraState() {
    if (!currentView())
        return;
    View3D::ViewportCameraState s;
    if (currentView()->cameraState(&s))
        QApplication::clipboard()->setText(serializeCameraState(s));
}
void ViewExtension::locateCameraExactly() {
    if (!currentView())
        return;
    View3D::ViewportCameraState s;
    if (!currentView()->cameraState(&s))
        return;
    QDialog d(m_window);
    d.setWindowTitle(QStringLiteral("精确定位相机"));
    auto *l = new QFormLayout(&d);
    QDoubleSpinBox *x[3];
    for (int i = 0; i < 3; ++i) {
        x[i] = new QDoubleSpinBox(&d);
        x[i]->setRange(-1e12, 1e12);
        x[i]->setDecimals(6);
        x[i]->setValue(s.position[i]);
        l->addRow(QStringLiteral("XYZ").mid(i, 1), x[i]);
    }
    auto *b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    l->addRow(b);
    QObject::connect(b, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(b, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() == QDialog::Accepted) {
        for (int i = 0; i < 3; ++i)
            s.position[i] = x[i]->value();
        currentView()->setCameraState(s);
    }
}
void ViewExtension::pasteCameraState() {
    if (!currentView())
        return;
    View3D::ViewportCameraState state;
    if (!parseCameraState(QApplication::clipboard()->text(), &state)) {
        QMessageBox::warning(m_window, QStringLiteral("粘贴相机状态"),
                             QStringLiteral("剪贴板中的相机状态格式无效"));
        return;
    }
    currentView()->setCameraState(state);
}
void ViewExtension::publishStates() {
    const QStringList ids = {QStringLiteral("cmd.environment.grid"),
                             QStringLiteral("cmd.environment.axes"),
                             QStringLiteral("cmd.environment.ambient_light"),
                             QStringLiteral("cmd.environment.main_light"),
                             QStringLiteral("cmd.environment.shadows"),
                             QStringLiteral("cmd.display.solid"),
                             QStringLiteral("cmd.display.wireframe"),
                             QStringLiteral("cmd.display.solid_wireframe"),
                             QStringLiteral("cmd.display.points"),
                             QStringLiteral("cmd.environment.background_solid"),
                             QStringLiteral("cmd.environment.background_gradient"),
                             QStringLiteral("cmd.projection.perspective"),
                             QStringLiteral("cmd.projection.orthographic"),
                             QStringLiteral("cmd.view.mode_2d"),
                             QStringLiteral("cmd.view.mode_3d"),
                             QStringLiteral("cmd.view.mode_globe")};
    for (const auto &id : ids)
        publishCommandState(id, {commandEnabled(id), commandChecked(id), true});
}
QString ViewExtension::serializeCameraState(const View3D::ViewportCameraState &s) {
    return QStringLiteral("projection=%1\nposition=%2,%3,%4\ntarget=%5,%6,%7\ndirection=%8,%9,%"
                          "10\nup=%11,%12,%13\nverticalFov=%14\northographicHeight=%15")
        .arg(s.orthographic ? QStringLiteral("orthographic") : QStringLiteral("perspective"))
        .arg(s.position[0], 0, 'g', 15)
        .arg(s.position[1], 0, 'g', 15)
        .arg(s.position[2], 0, 'g', 15)
        .arg(s.target[0], 0, 'g', 15)
        .arg(s.target[1], 0, 'g', 15)
        .arg(s.target[2], 0, 'g', 15)
        .arg(s.direction[0], 0, 'g', 15)
        .arg(s.direction[1], 0, 'g', 15)
        .arg(s.direction[2], 0, 'g', 15)
        .arg(s.up[0], 0, 'g', 15)
        .arg(s.up[1], 0, 'g', 15)
        .arg(s.up[2], 0, 'g', 15)
        .arg(s.verticalFovRadians, 0, 'g', 15)
        .arg(s.orthographicHeight, 0, 'g', 15);
}
bool ViewExtension::parseCameraState(const QString &text, View3D::ViewportCameraState *out) {
    if (!out)
        return false;
    QHash<QString, QString> values;
    for (const auto &line :
         text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const int p = line.indexOf(QLatin1Char('='));
        if (p <= 0)
            return false;
        values.insert(line.left(p).trimmed(), line.mid(p + 1).trimmed());
    }
    const auto vector = [&](const QString &key, std::array<double, 3> *v) {
        const auto parts = values.value(key).split(QLatin1Char(','));
        if (parts.size() != 3)
            return false;
        for (int i = 0; i < 3; ++i) {
            bool ok = false;
            (*v)[i] = parts[i].toDouble(&ok);
            if (!ok || !std::isfinite((*v)[i]))
                return false;
        }
        return true;
    };
    View3D::ViewportCameraState state;
    if (!vector(QStringLiteral("position"), &state.position) ||
        !vector(QStringLiteral("target"), &state.target) ||
        !vector(QStringLiteral("direction"), &state.direction) ||
        !vector(QStringLiteral("up"), &state.up))
        return false;
    bool fov = false, height = false;
    state.verticalFovRadians = values.value(QStringLiteral("verticalFov")).toDouble(&fov);
    state.orthographicHeight = values.value(QStringLiteral("orthographicHeight")).toDouble(&height);
    const auto projection = values.value(QStringLiteral("projection"));
    if (projection == QStringLiteral("orthographic"))
        state.orthographic = true;
    else if (projection == QStringLiteral("perspective"))
        state.orthographic = false;
    else
        return false;
    if (!fov || !height || state.verticalFovRadians <= .01 || state.verticalFovRadians >= 3.13 ||
        (state.orthographic && state.orthographicHeight <= 0))
        return false;
    *out = state;
    return true;
}
} // namespace View3DPlugins
