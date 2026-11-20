#include "LayoutConfig.h"
#include "CrashDiagnostics.h"
#ifdef VIEW3D_HOST_SMOKE_CHECKS
#include "HostSmokeChecks.h"
#endif
#include "DockLayoutUtils.h"
#include "FramelessEdgeFilter.h"
#include "PluginUiHost.h"
#include "HostDocks.h"
#include "LogDock.h"
#include "PluginManagerDock.h"
#include "ThemeController.h"
#include <Application.h>
#include <IExtension.h>
#include <ICommand.h>
#include <IContentsView.h>
#include <IDockWindow.h>

#include <IApplication.h>
#include <IDataOpen.h>
#include <IPlugin.h>
#include <IPluginRegistryService.h>
#include <ISceneTreeMenu.h>
#include <IViewportLayout.h>
#include <IViewportMenu.h>
#include <PluginServiceIds.h>
#include <View3DObjectIds.h>
#include <IMessageCenter.h>
#include <IWorkSpace.h>
#include <IUiObjectRegistry.h>
#include <MessageCenter.h>
#include <PluginConfigBuilder.h>
#include <PluginContainer.h>
#include <PluginFactoryRegistry.h>
#include <StatesManager.h>
#include <WorkSpace.h>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <SARibbonBar.h>
#include <SARibbonMainWindow.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLibrary>
#include <QList>
#include <QQueue>
#include <QSet>
#include <QMessageBox>
#include <QPixmap>
#include <QSplashScreen>
#include <QTimer>
#include <QCloseEvent>
#include <QSettings>
#include <QPointer>
#include <QSplitter>
#include <QByteArray>
#include <QStandardPaths>
#include <QStringList>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#include <utility>

namespace View3DApplication {
namespace {

bool findSplitterSlot(QWidget *widget, QSplitter **splitter, int *index)
{
    if (!widget)
        return false;
    QWidget *child = widget;
    while (child && child->parentWidget()) {
        QWidget *parent = child->parentWidget();
        if (auto *candidate = qobject_cast<QSplitter *>(parent)) {
            const int slot = candidate->indexOf(child);
            if (slot < 0)
                return false;
            *splitter = candidate;
            *index = slot;
            return true;
        }
        child = parent;
    }
    return false;
}

int centralSlotInSplitter(ads::CDockWidget *dock, QSplitter *splitter)
{
    auto *manager = dock ? dock->dockManager() : nullptr;
    auto *central = manager ? manager->centralWidget() : nullptr;
    if (!central)
        return -1;
    QWidget *child = central;
    while (child && child->parentWidget() != splitter)
        child = child->parentWidget();
    return child && child->parentWidget() == splitter ? splitter->indexOf(child) : -1;
}

} // namespace

void clampMeasurementDock(ads::CDockWidget *dock, QMainWindow *window)
{
    if (!dock || !window || dock->isClosed())
        return;
    const int widthLimit = qBound(360, window->width() / 3, 560);
    const int heightLimit = qBound(240, window->height() / 3, 420);
    if (dock->isFloating()) {
        QWidget *floating = dock->window();
        if (!floating || !floating->isVisible())
            return;
        const int width = floating->width();
        const int height = floating->height();
        if (width <= widthLimit && height <= heightLimit)
            return;
        const int targetWidth = qMin(width, widthLimit);
        const int targetHeight = qMin(height, heightLimit);
        floating->resize(targetWidth, targetHeight);
        std::cerr << "VIEW3D_DOCK_CLAMP name=dw.measurements floating=1 from=" << width
                  << "x" << height << " to=" << targetWidth << "x" << targetHeight
                  << std::endl;
        return;
    }
    auto *area = dock->dockAreaWidget();
    if (!area)
        return;
    // ADS 的 DockArea 可能嵌在容器里，不一定直接挂在 QSplitter 下；向上找到
    // 最近的分隔条，并取它直接包含 area 的那一层子控件作为尺寸槽。
    QWidget *splitterChild = area;
    QSplitter *splitter = nullptr;
    while (splitterChild && splitterChild->parentWidget()) {
        QWidget *parent = splitterChild->parentWidget();
        if (auto *candidate = qobject_cast<QSplitter *>(parent)) {
            splitter = candidate;
            break;
        }
        splitterChild = parent;
    }
    if (!splitter)
        return;
    const int index = splitter->indexOf(splitterChild);
    if (index < 0)
        return;
    QList<int> sizes = splitter->sizes();
    if (sizes.size() <= index)
        return;
    const int limit = splitter->orientation() == Qt::Horizontal ? widthLimit : heightLimit;
    if (sizes[index] <= limit)
        return;
    const int excess = sizes[index] - limit;
    sizes[index] = limit;
    // 收回的空间只还给中央三维视图，不能摊给左侧场景树等其它面板。
    int recipient = -1;
    if (auto *manager = dock->dockManager()) {
        if (auto *central = manager->centralWidget()) {
            QWidget *child = central;
            while (child && child->parentWidget() != splitter)
                child = child->parentWidget();
            if (child && child->parentWidget() == splitter)
                recipient = splitter->indexOf(child);
        }
    }
    if (recipient < 0 || recipient == index) {
        for (int i = 0; i < sizes.size(); ++i) {
            if (i == index)
                continue;
            if (recipient < 0 || sizes[i] > sizes[recipient])
                recipient = i;
        }
    }
    if (recipient >= 0 && recipient != index)
        sizes[recipient] += excess;
    splitter->setSizes(sizes);
    std::cerr << "VIEW3D_DOCK_CLAMP name=dw.measurements limit=" << limit << " sizes=";
    for (int size : sizes)
        std::cerr << size << ',';
    std::cerr << std::endl;
}

void restoreSceneDockWidth(ads::CDockWidget *sceneDock, int width, QMainWindow *window)
{
    if (!sceneDock || !window || width <= 0 || sceneDock->isClosed())
        return;
    if (sceneDock->isFloating()) {
        QWidget *floating = sceneDock->window();
        if (floating && floating->isVisible() && floating->width() != width)
            floating->resize(width, floating->height());
        return;
    }
    auto *area = sceneDock->dockAreaWidget();
    QSplitter *splitter = nullptr;
    int sceneIndex = -1;
    if (!findSplitterSlot(area, &splitter, &sceneIndex))
        return;
    const int centralIndex = centralSlotInSplitter(sceneDock, splitter);
    if (centralIndex < 0 || centralIndex == sceneIndex)
        return;
    QList<int> sizes = splitter->sizes();
    if (sceneIndex >= sizes.size() || centralIndex >= sizes.size())
        return;
    if (sizes[sceneIndex] == width)
        return;
    const int delta = width - sizes[sceneIndex];
    if (sizes[centralIndex] - delta < 1)
        return;
    const int previous = sizes[sceneIndex];
    sizes[sceneIndex] = width;
    sizes[centralIndex] -= delta;
    splitter->setSizes(sizes);
    std::cerr << "VIEW3D_DOCK_KEEP_LEFT from=" << previous << " to=" << width
              << " central=" << sizes[centralIndex] << std::endl;
}

// 记住左侧场景树宽度；测量面板停靠/浮动变化后把它恢复，避免 ADS 新建右侧
// 停靠区时顺手压缩左侧面板。
class DockSizeGuard final : public QObject
{
  public:
    DockSizeGuard(ads::CDockManager *manager, QMainWindow *window, QObject *parent)
        : QObject(parent), m_manager(manager), m_window(window)
    {
        m_sceneDock = manager ? manager->findDockWidget(QStringLiteral("dw.scene")) : nullptr;
        m_measurementDock =
            manager ? manager->findDockWidget(QStringLiteral("dw.measurements")) : nullptr;
        m_recordTimer.setInterval(300);
        connect(&m_recordTimer, &QTimer::timeout, this, [this] { record(); });
        m_recordTimer.start();
        QTimer::singleShot(0, this, [this] { record(); });
    }

    void schedule()
    {
        if (m_pending)
            return;
        m_pending = true;
        QTimer::singleShot(0, this, [this] { apply(); });
        QTimer::singleShot(150, this, [this] { apply(); });
    }

    void apply()
    {
        restore();
        if (m_measurementDock && m_window)
            View3DApplication::clampMeasurementDock(m_measurementDock, m_window);
        m_pending = false;
    }

  private:
    void record()
    {
        if (m_pending || !m_sceneDock)
            return;
        if (m_sceneDock->isFloating()) {
            QWidget *floating = m_sceneDock->window();
            if (floating && floating->isVisible() && floating->width() > 0)
                m_sceneWidth = floating->width();
            return;
        }
        QSplitter *splitter = nullptr;
        int index = -1;
        if (!findSplitterSlot(m_sceneDock->dockAreaWidget(), &splitter, &index))
            return;
        const QList<int> sizes = splitter->sizes();
        if (index < sizes.size() && sizes[index] > 0) {
            int width = sizes[index];
            // 已被旧的停靠操作压缩到不可用宽度时，启动先恢复到可用宽度。
            if (width < 180) {
                const int corrected = qMin(240, qMax(180,
                    m_window ? m_window->width() / 3 : 240));
                std::cerr << "VIEW3D_DOCK_KEEP_LEFT baseline=" << width
                          << " corrected=" << corrected << std::endl;
                width = corrected;
            }
            m_sceneWidth = width;
        }
    }

    void restore()
    {
        if (!m_sceneDock || m_sceneWidth <= 0)
            return;
        View3DApplication::restoreSceneDockWidth(m_sceneDock, m_sceneWidth, m_window);
    }

    ads::CDockManager *m_manager = nullptr;
    QMainWindow *m_window = nullptr;
    QPointer<ads::CDockWidget> m_sceneDock;
    QPointer<ads::CDockWidget> m_measurementDock;
    QTimer m_recordTimer;
    int m_sceneWidth = 0;
    bool m_pending = false;
};

} // namespace View3DApplication

namespace {

struct HostMessageOwner final : RF::PluginEngine::IPlugin {};

// 场景树右键菜单的贡献者注册表由宿主在插件创建前发布：数据插件（如倾斜摄影）
// 比场景视图插件先创建，不能依赖扩展初始化顺序来拿到注册表。
class SceneTreeMenuRegistry final : public View3D::ISceneTreeMenuRegistry {
  public:
    void addSceneTreeMenuContributor(View3D::ISceneTreeMenuContributor *contributor) override
    {
        if (contributor && !m_contributors.contains(contributor))
            m_contributors.append(contributor);
    }
    void removeSceneTreeMenuContributor(View3D::ISceneTreeMenuContributor *contributor) override
    {
        m_contributors.removeAll(contributor);
    }
    QList<View3D::ISceneTreeMenuContributor *> sceneTreeMenuContributors() const override
    {
        return m_contributors;
    }

  private:
    QList<View3D::ISceneTreeMenuContributor *> m_contributors;
};

// 通用“打开数据”入口的打开器注册表：数据插件登记自己的文件类型与处理器，
// 目录树只负责统一菜单与文件选择。与菜单注册表一样由宿主提前发布。
// 三维视口右键菜单的贡献者注册表：视图插件只负责弹出，具体工具项由工具插件登记。
class ViewportMenuRegistry final : public View3D::IViewportMenuRegistry {
  public:
    void addViewportMenuContributor(View3D::IViewportMenuContributor *contributor) override
    {
        if (contributor && !m_contributors.contains(contributor))
            m_contributors.append(contributor);
    }
    void removeViewportMenuContributor(View3D::IViewportMenuContributor *contributor) override
    {
        m_contributors.removeAll(contributor);
    }
    QList<View3D::IViewportMenuContributor *> viewportMenuContributors() const override
    {
        return m_contributors;
    }

  private:
    QList<View3D::IViewportMenuContributor *> m_contributors;
};

class DataOpenRegistry final : public View3D::IDataOpenRegistry {
  public:
    void addDataOpener(View3D::IDataOpener *opener) override
    {
        if (opener && !m_openers.contains(opener))
            m_openers.append(opener);
    }
    void removeDataOpener(View3D::IDataOpener *opener) override
    {
        m_openers.removeAll(opener);
    }
    QList<View3D::IDataOpener *> dataOpeners() const override { return m_openers; }

  private:
    QList<View3D::IDataOpener *> m_openers;
};

class UiObjectRegistry final : public RF::PluginEngine::IUiObjectRegistry {
  public:
    QAction *action(const QString &id) const override {
        return m_actions.value(id, nullptr);
    }

    QWidget *widget(const QString &id) const override {
        return m_widgets.value(id, nullptr);
    }

    void registerAction(const QString &id, QAction *action) override {
        if (!id.isEmpty() && action)
            m_actions.insert(id, action);
    }

    void registerWidget(const QString &id, QWidget *widget) override {
        if (!id.isEmpty() && widget)
            m_widgets.insert(id, widget);
    }
    void unregisterAction(const QString &id, QAction *action) override {
        if (m_actions.value(id) == action)
            m_actions.remove(id);
    }
    void unregisterWidget(const QString &id, QWidget *widget) override {
        if (m_widgets.value(id) == widget)
            m_widgets.remove(id);
    }

  private:
    QHash<QString, QAction *> m_actions;
    QHash<QString, QWidget *> m_widgets;
};

class PluginRegistryServiceImpl final : public RF::PluginEngine::IPluginRegistryService {
  public:
    void configure(const QList<View3D::PackageLayout> &layouts,
                   const QSet<QString> &loadedIds,
                   const QHash<QString, QString> &loadErrors) {
        m_layouts = layouts;
        m_loadedIds = loadedIds;
        m_loadErrors = loadErrors;
    }
    QList<RF::PluginEngine::PluginPackageStatus> packages() const override {
        QList<RF::PluginEngine::PluginPackageStatus> result;
        result.reserve(m_layouts.size());
        for (const View3D::PackageLayout &layout : m_layouts) {
            RF::PluginEngine::PluginPackageStatus status;
            status.id = layout.id;
            status.path = layout.path;
            status.required = layout.required;
            status.enabled = layout.enabled;
            status.requiresPlugins = layout.requiresPlugins;
            status.optionalPlugins = layout.optionalPlugins;
            status.provides = layout.provides;
            status.loaded = m_loadedIds.contains(layout.id);
            status.error = m_loadErrors.value(layout.id);
            result.append(std::move(status));
        }
        return result;
    }

  private:
    QList<View3D::PackageLayout> m_layouts;
    QSet<QString> m_loadedIds;
    QHash<QString, QString> m_loadErrors;
};

QString layoutStateFilePath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("layout_state.ini"));
}

// 当前配置里实际存在的面板名字集合，用来判断存档的停靠状态是否已经过期。
// 前缀的布局版本号在「已保存的状态本身可能被写坏」时手动 +1：版本号一变，所有旧存档一律
// 作废重建，不用挨个让用户删 layout_state.ini。
//   1 初版；2 加入 dock 名字指纹；3 撤掉强行显示标题栏的看门狗（它会在布局中途改变区域
//   尺寸线索，把几何写坏，导致面板内容画到窗口外面）。
QString dockLayoutFingerprint(const QList<View3D::DockLayoutEntry> &docks,
                              const QList<View3D::ContentsViewLayoutEntry> &contentsViews) {
    QStringList names;
    for (const auto &entry : docks)
        names.append(entry.name);
    for (const auto &entry : contentsViews)
        names.append(entry.name);
    names.sort(Qt::CaseInsensitive);
    return QStringLiteral("v3;") + names.join(QLatin1Char(';'));
}

// 测量结果表的内容很长，拖到右侧时 ADS 可能按内容尺寸把面板撑到接近整窗。
// 停靠/恢复/浮动切换后按窗口尺寸给一个上限，保留用户拖到的位置但收回异常宽度。
// 拖拽停靠不保证触发 CDockManager 的信号；监听测量面板的 ParentChange/Show，
// 在停靠或浮出后延迟把异常尺寸收回来。
class DockClampWatcher final : public QObject
{
  public:
    DockClampWatcher(std::function<void()> callback, QObject *parent)
        : QObject(parent), m_callback(std::move(callback))
    {
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ParentChange || event->type() == QEvent::Show) {
            QTimer::singleShot(0, this, [this] { m_callback(); });
            QTimer::singleShot(150, this, [this] { m_callback(); });
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    std::function<void()> m_callback;
};

class WindowStateManager final : public QObject {
  public:
    WindowStateManager(QMainWindow *window, ads::CDockManager *dockManager,
                       QString layoutFingerprint, QObject *parent = nullptr)
        : QObject(parent), m_window(window), m_dockManager(dockManager),
          m_layoutFingerprint(std::move(layoutFingerprint)) {
        if (m_window)
            m_window->installEventFilter(this);
    }
    void restore() {
        if (!m_window || !m_dockManager)
            return;
        QSettings settings(layoutStateFilePath(), QSettings::IniFormat);
        // 面板集合一变（增删停靠窗口、改 ui_config.xml），旧状态里那些已经不存在的
        // 面板会留下占位的空区域，把还在的面板挤到中间甚至挤没内容。指纹不符就整份
        // 丢弃，退回按配置新建的默认布局。
        if (settings.value(QStringLiteral("ui/dockLayoutFingerprint")).toString() !=
            m_layoutFingerprint) {
            settings.remove(QStringLiteral("ui/dockState"));
        }
        const QByteArray geometry = settings.value(QStringLiteral("ui/windowGeometry"))
                                        .toByteArray();
        const QByteArray dockState = settings.value(QStringLiteral("ui/dockState"))
                                         .toByteArray();
        if (!geometry.isEmpty())
            m_window->restoreGeometry(geometry);
        if (!dockState.isEmpty())
            m_dockManager->restoreState(dockState);
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_window && event->type() == QEvent::Close) {
            QSettings settings(layoutStateFilePath(), QSettings::IniFormat);
            settings.setValue(QStringLiteral("ui/windowGeometry"), m_window->saveGeometry());
            if (m_dockManager) {
                settings.setValue(QStringLiteral("ui/dockState"), m_dockManager->saveState());
                settings.setValue(QStringLiteral("ui/dockLayoutFingerprint"),
                                  m_layoutFingerprint);
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QMainWindow *m_window = nullptr;
    ads::CDockManager *m_dockManager = nullptr;
    QString m_layoutFingerprint;
};

class ApplicationRuntime final : public RF::PluginEngine::Application {
  public:
    ApplicationRuntime() {
        setPluginContainer(&m_plugins);
        setArgs(QCoreApplication::arguments());
    }
    RF::PluginEngine::IUiObjectRegistry *getUiObjectRegistry() override {
        return &m_uiObjects;
    }
    RF::PluginEngine::IPlugin *getPluginObject(const QString &name) const override {
        return static_cast<RF::PluginEngine::IPlugin *>(m_plugins.object(name));
    }
    QMainWindow *getMainWindow() const override {
        return m_window;
    }
    void closeApplication() override {
        QCoreApplication::quit();
    }
    QString getTitle() const override {
        return m_window ? m_window->windowTitle()
                        : RF::PluginEngine::Application::getTitle();
    }
    void setTitle(const QString &title) override {
        if (m_window)
            m_window->setWindowTitle(title);
        RF::PluginEngine::Application::setTitle(title);
    }

    void setMainWindow(QMainWindow *window) {
        m_window = window;
    }
    void configurePluginRegistry(const QList<View3D::PackageLayout> &layouts,
                                 const QSet<QString> &loadedIds,
                                 const QHash<QString, QString> &loadErrors) {
        m_pluginRegistry.configure(layouts, loadedIds, loadErrors);
        m_plugins.publishAs<RF::PluginEngine::IPluginRegistryService>(
            RF::PluginEngine::PluginRegistryServiceId, &m_pluginRegistry);
    }
  private:
    QMainWindow *m_window = nullptr;
    RF::PluginEngine::PluginContainer m_plugins;
    UiObjectRegistry m_uiObjects;
    PluginRegistryServiceImpl m_pluginRegistry;
};

struct LoadedPackage {
    QString configuredId;
    std::unique_ptr<QLibrary> library;
};

using RegisterPluginFactories = void (*)(RF::PluginEngine::PluginFactoryRegistry *);
using PluginId = const char *(*)();

QString resolvePath(const QDir &baseDirectory, const QString &path) {
    return QDir::cleanPath(QFileInfo(path).isAbsolute() ? path : baseDirectory.filePath(path));
}

bool loadPackage(const View3D::PackageLayout &package, const QDir &configDirectory,
                 RF::PluginEngine::PluginFactoryRegistry *factories, LoadedPackage *loaded,
                 QString *error) {
    auto library = std::make_unique<QLibrary>(resolvePath(configDirectory, package.path));
    if (!library->load()) {
        if (error)
            *error =
                QStringLiteral("无法加载插件包 %1：%2").arg(package.id, library->errorString());
        return false;
    }

    const auto pluginId = reinterpret_cast<PluginId>(library->resolve("view3dPluginId"));
    auto registerFactories =
        reinterpret_cast<RegisterPluginFactories>(library->resolve("registerPluginFactories"));
    if (!registerFactories) {
        registerFactories =
            reinterpret_cast<RegisterPluginFactories>(library->resolve("RegisterPluginFactories"));
    }
    if (!pluginId || !registerFactories) {
        if (error)
            *error = QStringLiteral("插件包 %1 缺少必需导出函数").arg(package.id);
        return false;
    }

    const QString actualId = QString::fromUtf8(pluginId());
    if (actualId != package.id) {
        if (error) {
            *error = QStringLiteral("插件包不匹配：配置 %1，实际 %2")
                         .arg(package.id)
                         .arg(actualId);
        }
        return false;
    }

    registerFactories(factories);
    loaded->configuredId = package.id;
    loaded->library = std::move(library);
    return true;
}

QString commandLineConfigPath(const QStringList &arguments, const QString &option,
                              const QString &fallback) {
    for (int index = 0; index < arguments.size(); ++index) {
        const QString argument = arguments[index];
        const QString prefix = option + QLatin1Char('=');
        if (argument.startsWith(prefix))
            return QFileInfo(argument.mid(prefix.size())).absoluteFilePath();
        if (argument == option && index + 1 < arguments.size())
            return QFileInfo(arguments[index + 1]).absoluteFilePath();
    }
    return fallback;
}


} // namespace

extern "C" __declspec(dllexport) int runConfiguredApplication(int argc, char *argv[]) {
    // Ogre 视口是往原生 Win32 子句柄上画 D3D 的。默认情况下 Qt 会因为这一个原生子窗口
    // 把它的**所有兄弟控件**也变成原生句柄（实测主窗口下挂了 55 个 Qt 子句柄）。把面板
    // 浮动出去时，这些原生控件要在两个顶层窗口之间重新设置父级，Qt 处理不干净，浮出来的
    // 窗口就会出现内容整体上移、右侧和底部留黑块不重绘。
    // 关掉「兄弟控件跟随原生」之后，只有视口本身是原生句柄，面板仍然是 Qt 的 alien widget，
    // 重新设置父级就走正常路径。必须在 QApplication 构造之前设置。
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    application.setOrganizationName(QStringLiteral("View3D"));
    application.setApplicationName(QStringLiteral("View3D"));
    const QString applicationDataDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    application.setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    const QString defaultConfig =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ui_config.xml"));
    const QString configPath = commandLineConfigPath(application.arguments(),
                                                     QStringLiteral("--ui-config"), defaultConfig);
    const bool smokeTest = application.arguments().contains(QStringLiteral("--smoke-test"));
    const auto reportFatal = [&](const QString &title, const QString &message) {
        std::cerr << title.toStdString() << ": " << message.toStdString() << '\n';
        if (!smokeTest)
            QMessageBox::critical(nullptr, title, message);
    };
    QString layoutWarning;
    const View3D::LayoutConfig layout =
        View3D::LayoutConfig::load(configPath, &layoutWarning);
    QStringList pluginIds;
    for (const auto &package : layout.packages)
        pluginIds.append(package.id);
    View3DApplication::installCrashDiagnostics(
        applicationDataDirectory, pluginIds.join(QStringLiteral(", ")));
    const QDir configDirectory = QFileInfo(configPath).absoluteDir();
    for (const QString &path : layout.libraryPaths)
        QCoreApplication::addLibraryPath(resolvePath(configDirectory, path));

    RF::PluginEngine::PluginFactoryRegistry factories;
    ApplicationRuntime runtime;
    // Qt 自己的告警也进日志面板（仍然转交原 handler，stderr 输出不变）；
    // 必须在 runtime 析构前卸载，否则回调会碰到已释放的日志管理器。
    View3DApplication::installQtMessageLogBridge(runtime.getAppLogManager());
    SceneTreeMenuRegistry sceneTreeMenuRegistry;
    DataOpenRegistry dataOpenRegistry;
    ViewportMenuRegistry viewportMenuRegistry;
    if (auto *container = runtime.getPluginContainer()) {
        container->publishAs<View3D::ISceneTreeMenuRegistry>(
            View3D::ServiceIds::SceneTreeMenu, &sceneTreeMenuRegistry);
        container->publishAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen, &dataOpenRegistry);
        container->publishAs<View3D::IViewportMenuRegistry>(
            View3D::ServiceIds::ViewportMenu, &viewportMenuRegistry);
    }
    std::vector<LoadedPackage> packages;
    packages.reserve(static_cast<std::size_t>(layout.packages.size()));
    QSet<QString> loadedPluginIds;
    QStringList warnings;
    std::vector<std::unique_ptr<RF::PluginEngine::IPlugin>> pluginObjects;
    if (!layoutWarning.isEmpty())
        warnings.append(layoutWarning);

    QList<View3D::PackageLayout> orderedPackages;
    QHash<QString, View3D::PackageLayout> packagesById;
    for (const auto &package : layout.packages)
        packagesById.insert(package.id, package);
    QSet<QString> emitted;
    while (orderedPackages.size() < layout.packages.size()) {
        bool progressed = false;
        for (const auto &package : layout.packages) {
            if (emitted.contains(package.id))
                continue;
            bool ready = true;
            for (const QString &dependency : package.requiresPlugins) {
                if (!packagesById.contains(dependency)) {
                    const QString message =
                        QStringLiteral("插件 %1 缺少必需依赖 %2").arg(package.id, dependency);
                    if (package.required) {
                        reportFatal(QStringLiteral("插件依赖错误"), message);
                        return 30;
                    }
                    warnings.append(message);
                    ready = false;
                    break;
                }
                if (!emitted.contains(dependency)) {
                    ready = false;
                    break;
                }
            }
            if (ready)
                for (const QString &dependency : package.optionalPlugins)
                    if (packagesById.contains(dependency) && !emitted.contains(dependency)) {
                        ready = false;
                        break;
                    }
            if (ready) {
                orderedPackages.append(package);
                emitted.insert(package.id);
                progressed = true;
            }
        }
        if (!progressed) {
            reportFatal(QStringLiteral("插件依赖错误"),
                        QStringLiteral("检测到插件循环依赖或无法满足的依赖"));
            return 30;
        }
    }
    QHash<QString, QString> packageErrors;
    for (const View3D::PackageLayout &package : orderedPackages) {
        if (!package.enabled) {
            packageErrors.insert(package.id, QStringLiteral("插件已禁用"));
            continue;
        }
        QStringList unavailableDependencies;
        for (const QString &dependency : package.requiresPlugins)
            if (!loadedPluginIds.contains(dependency))
                unavailableDependencies.append(dependency);
        if (!unavailableDependencies.isEmpty()) {
            const QString message =
                QStringLiteral("插件 %1 的依赖未成功加载：%2")
                    .arg(package.id, unavailableDependencies.join(QStringLiteral(", ")));
            warnings.append(message);
            packageErrors.insert(package.id, message);
            if (package.required) {
                reportFatal(QStringLiteral("插件依赖错误"), message);
                return 31;
            }
            continue;
        }
        LoadedPackage loaded;
        QString error;
        if (!loadPackage(package, configDirectory, &factories, &loaded, &error)) {
            warnings.append(error);
            packageErrors.insert(package.id, error);
            if (package.required) {
                reportFatal(QStringLiteral("插件加载失败"), error);
                return 31;
            }
            continue;
        }
        loadedPluginIds.insert(loaded.configuredId);
        packages.push_back(std::move(loaded));
    }
    runtime.configurePluginRegistry(orderedPackages, loadedPluginIds, packageErrors);

    SARibbonMainWindow window;
    // 缩放带要让无边框助手先拿到，但 ribbon 右上角按钮、停靠区这些子控件会把边缘上的
    // 按下吞掉；过滤器装在应用级才能抢在控件和助手前面。
    View3DApplication::FramelessEdgeFilter edgeFilter(&window);
    runtime.setMainWindow(&window);
    window.setWindowTitle(layout.caption);
    window.resize(layout.windowWidth, layout.windowHeight);
    if (layout.windowX >= 0 && layout.windowY >= 0)
        window.move(layout.windowX, layout.windowY);
    QString themeSettingsPath = layoutStateFilePath();
    if (smokeTest) {
        themeSettingsPath = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("View3D-theme-smoke-%1.ini")
                                              .arg(QCoreApplication::applicationPid()));
        QFile::remove(themeSettingsPath);
    }
    View3DApplication::ThemeController themeController(window, themeSettingsPath, true);
    themeController.initialize(layout.ribbonStyle, layout.ribbonTheme, &warnings);
    View3DApplication::registerThemeCommandFactory(factories, themeController);
    View3DApplication::registerHostDockFactory(factories);

    // 宿主自带命令（主题切换等）不属于任何插件包，但同样由 ui_config 声明。
    const auto isActivePlugin = [&loadedPluginIds](const QString &pluginId) {
        return pluginId == View3DApplication::hostCommandPluginId() ||
            loadedPluginIds.contains(pluginId);
    };
    QList<View3D::RibbonPageLayout> activeRibbonPages;
    for (const auto &page : layout.ribbonPages) {
        View3D::RibbonPageLayout activePage = page;
        activePage.groups.clear();
        for (const auto &group : page.groups) {
            View3D::RibbonGroupLayout activeGroup = group;
            activeGroup.items.clear();
            for (const auto &item : group.items)
                if (isActivePlugin(item.pluginId))
                    activeGroup.items.append(item);
            if (!activeGroup.items.isEmpty())
                activePage.groups.append(activeGroup);
        }
        if (!activePage.groups.isEmpty())
            activeRibbonPages.append(activePage);
    }
    QList<View3D::RibbonGroupLayout::Item> activeRightButtons;
    for (const auto &item : layout.ribbonRightButtons)
        if (isActivePlugin(item.pluginId))
            activeRightButtons.append(item);
    QList<View3D::ContentsViewLayoutEntry> activeContentsViews;
    for (const auto &entry : layout.contentsViews)
        if (isActivePlugin(entry.pluginId))
            activeContentsViews.append(entry);
    QList<View3D::DockLayoutEntry> activeDocks;
    for (const auto &entry : layout.docks)
        if (isActivePlugin(entry.pluginId))
            activeDocks.append(entry);

    View3D::LayoutConfig activeLayout = layout;
    activeLayout.ribbonPages = activeRibbonPages;
    activeLayout.ribbonRightButtons = activeRightButtons;
    activeLayout.contentsViews = activeContentsViews;
    activeLayout.docks = activeDocks;

    RF::PluginEngine::PluginConfigBuilder::Result configObjects =
        RF::PluginEngine::PluginConfigBuilder::buildFromLayout(
            activeLayout, &runtime, runtime.getPluginContainer(), &factories);
    warnings.append(configObjects.warnings);

    std::unique_ptr<QSplashScreen> welcome;
    if (!smokeTest &&
        layout.welcomeScreen.value(QStringLiteral("enable")) == QStringLiteral("true")) {
        QPixmap image(
            resolvePath(configDirectory, layout.welcomeScreen.value(QStringLiteral("image"))));
        if (!image.isNull()) {
            welcome = std::make_unique<QSplashScreen>(image);
            welcome->show();
            application.processEvents();
        }
    }

    int result = 0;
    {
        ads::CDockManager dockManagerStorage(&window);
        auto *dockManager = &dockManagerStorage;
        View3DApplication::PluginUiHost uiHost(window, runtime, loadedPluginIds);

        if (!uiHost.buildRibbon(window.ribbonBar(), activeRibbonPages,
                                activeRightButtons, configDirectory, &warnings) ||
            !uiHost.buildContentsViews(dockManager, activeContentsViews, configDirectory,
                                       &warnings) ||
            !uiHost.buildDockWindows(dockManager, activeDocks, configDirectory, &warnings)) {
            reportFatal(QStringLiteral("应用启动失败"), warnings.join(QLatin1Char('\n')));
            result = 34;
        } else {
            themeController.applyStyle(window.ribbonBar());
            themeController.applyTheme();
            // SARibbon 的主题在窗口构造期设置可能不完整，事件循环启动后再落一次。
            QTimer::singleShot(0, &window,
                               [&themeController] { themeController.applyTheme(); });
            auto windowState = std::make_unique<WindowStateManager>(
                &window, dockManager, dockLayoutFingerprint(activeDocks, activeContentsViews));
            const QByteArray defaultDockState = dockManager->saveState();
            const QByteArray defaultGeometry = window.saveGeometry();
            windowState->restore();
            HostMessageOwner layoutOwner;
            runtime.getMessageCenter()->subscribe(
                QStringLiteral("ui.reset_layout"), &layoutOwner,
                [&window, dockManager, defaultDockState, defaultGeometry](
                    RF::PluginEngine::Message &) {
                    QSettings settings(layoutStateFilePath(), QSettings::IniFormat);
                    settings.remove(QStringLiteral("ui/windowGeometry"));
                    settings.remove(QStringLiteral("ui/dockState"));
                    settings.remove(QStringLiteral("ui/dockLayoutFingerprint"));
                    window.restoreGeometry(defaultGeometry);
                    dockManager->restoreState(defaultDockState);
                });
            runtime.getMessageCenter()->subscribe(
                QStringLiteral("ui.context_changed"), &layoutOwner,
                [&uiHost](RF::PluginEngine::Message &message) {
                    uiHost.setContextVisible(
                        message.arguments.value(QStringLiteral("context")).toString());
                });
            // 命令的可用/勾选状态只有在收到这个广播时才回读，Esc 退出工具、
            // 右键菜单完成折线这类「不是点按钮引起的状态变化」才能立刻反映到 ribbon。
            runtime.getMessageCenter()->subscribe(
                QStringLiteral("ui.refresh_command_states"), &layoutOwner,
                [&uiHost](RF::PluginEngine::Message &) { uiHost.refreshCommandStates(); });
            if (smokeTest)
                std::cerr << "VIEW3D_EXTENSION_SETUP_BEGIN\n";
            auto pendingExtensions = factories.creationContexts(QStringLiteral("extension"));
            pendingExtensions.append(factories.creationContexts(QStringLiteral("object")));
            for (int round = 0; round < 4 && !pendingExtensions.isEmpty(); ++round) {
                QList<RF::PluginEngine::PluginCreationContext> retry;
                int created = 0;
                for (const auto &context : std::as_const(pendingExtensions)) {
                    if (smokeTest)
                        std::cerr << "VIEW3D_EXTENSION_BEGIN "
                                  << context.pluginId.toStdString() << "/"
                                  << context.factoryId.toStdString() << '\n';
                    std::unique_ptr<RF::PluginEngine::IPlugin> extension(factories.create(context));
                    if (!extension)
                        continue;
                    auto *typed = dynamic_cast<RF::PluginEngine::IExtension *>(extension.get());
                    const bool initialized = !typed || typed->onCreate(&runtime);
                    if (!initialized) {
                        if (typed) {
                            typed->onDestroy();
                            // 兜底清理：扩展没有显式 unpublish 的服务随扩展一起消失。
                            runtime.getPluginContainer()->unpublishObjectsOwnedBy(
                                extension.get());
                        }
                        if (smokeTest)
                            std::cerr << "VIEW3D_EXTENSION_RETRY\n";
                        retry.append(context);
                        continue;
                    }
                    runtime.getPluginContainer()->publishAs<RF::PluginEngine::IPlugin>(
                        context.pluginId + QStringLiteral(".") + context.factoryId,
                        extension.get(), extension.get());
                    pluginObjects.push_back(std::move(extension));
                    ++created;
                    if (smokeTest)
                        std::cerr << "VIEW3D_EXTENSION_READY\n";
                }
                pendingExtensions = retry;
                if (!created)
                    break;
            }
            for (const auto &context : std::as_const(pendingExtensions))
                warnings.append(QStringLiteral("Extension initialization failed: %1/%2")
                                    .arg(context.pluginId, context.factoryId));
            if (!smokeTest && !activeContentsViews.isEmpty()) {
                auto *viewportLayout = runtime.getPluginContainer()
                    ? runtime.getPluginContainer()->objectAs<View3D::IViewportLayout>(
                          View3D::ServiceIds::ViewportLayout)
                    : nullptr;
                if (!viewportLayout || !viewportLayout->primaryView()) {
                    QMessageBox::critical(
                        &window, QStringLiteral("三维视口启动失败"),
                        QStringLiteral("渲染服务或三维视口未初始化，请检查 plugins 目录、插件加载状态与 system_config.xml。"));
                }
            }
            // 刷新命令状态要等窗口 show 之后：命令的 getChecked() 读 QWidget::isVisible()，
            // show 之前一律是 false，轮询会把可勾选的东西统统关掉。冒烟模式不显示窗口，
            // 只能在这里刷新一次。
            if (smokeTest) {
#ifdef VIEW3D_HOST_SMOKE_CHECKS
                uiHost.refreshCommandStates();
                QString uiError;
                const bool uiOk = View3DApplication::runHostSmokeChecks(
                    themeController, window, uiHost, dockManager, runtime,
                    activeRibbonPages, activeRightButtons, activeDocks, activeContentsViews,
                    configPath, themeSettingsPath, &uiError);
                if (!uiOk) {
                    QTimer::singleShot(0, &application,
                                       [&application] { application.exit(42); });
                } else {
                    std::cerr << "VIEW3D_UI_CHECKS_OK\n";
                }
#endif
                QFile::remove(themeSettingsPath);
                std::cerr << "VIEW3D_EXTENSION_SETUP_DONE\n";
            }
            if (!smokeTest) {
                if (layout.maximized)
                    window.showMaximized();
                else
                    window.show();
                // 新布局（如新增停靠面板）第一次启动时，窗口显示/最大化/停靠恢复可能
                // 在本事件之后才把视口尺寸定下来；补两次分辨率同步，避免首屏被拉伸。
                const auto syncRenderResolution = [&runtime] {
                    auto* container = runtime.getPluginContainer();
                    auto* viewportLayout = container
                        ? container->objectAs<View3D::IViewportLayout>(
                              View3D::ServiceIds::ViewportLayout)
                        : nullptr;
                    if (!viewportLayout)
                        return;
                    for (View3D::IRenderSurface* view : viewportLayout->views()) {
                        if (view)
                            view->refreshRenderResolution();
                    }
                };
                QTimer::singleShot(0, &window, syncRenderResolution);
                QTimer::singleShot(250, &window, syncRenderResolution);
                auto *dockSizeGuard = new View3DApplication::DockSizeGuard(
                    dockManager, &window, &window);
                const auto scheduleDockGuard = [dockSizeGuard] {
                    dockSizeGuard->schedule();
                };
                QObject::connect(dockManager, &ads::CDockManager::dockAreaCreated,
                                 &window,
                                 [scheduleDockGuard](ads::CDockAreaWidget *) {
                                     scheduleDockGuard();
                                 });
                QObject::connect(dockManager, &ads::CDockManager::dockWidgetAdded,
                                 &window,
                                 [scheduleDockGuard](ads::CDockWidget *) {
                                     scheduleDockGuard();
                                 });
                QObject::connect(dockManager, &ads::CDockManager::floatingWidgetCreated,
                                 &window,
                                 [scheduleDockGuard](ads::CFloatingDockContainer *) {
                                     scheduleDockGuard();
                                 });
                QObject::connect(dockManager, &ads::CDockManager::stateRestored, &window,
                                 [scheduleDockGuard] { scheduleDockGuard(); });
                if (auto *measurementDock =
                        dockManager->findDockWidget(QStringLiteral("dw.measurements"))) {
                    QObject::connect(measurementDock, &ads::CDockWidget::viewToggled,
                                     &window,
                                     [scheduleDockGuard](bool) { scheduleDockGuard(); });
                    auto *watcher =
                        new DockClampWatcher(scheduleDockGuard, measurementDock);
                    measurementDock->installEventFilter(watcher);
                }
                QTimer::singleShot(0, &window, scheduleDockGuard);
                QTimer::singleShot(250, &window, scheduleDockGuard);
                QTimer::singleShot(600, &window, scheduleDockGuard);
                if (welcome)
                    welcome->finish(&window);
            }
            if (!warnings.isEmpty())
                std::cerr << warnings.join(QLatin1Char('\n')).toStdString() << '\n';
            if (smokeTest) {
#ifdef VIEW3D_HOST_SMOKE_CHECKS
                View3DApplication::startSmokeWait(application, runtime);
#else
                QTimer::singleShot(0, &application, &QCoreApplication::quit);
#endif
            } else {
                QTimer::singleShot(0, &window, [&uiHost] { uiHost.refreshCommandStates(); });
            }
            if (smokeTest)
                std::cerr << "VIEW3D_EVENT_LOOP_BEGIN\n";
            result = application.exec();
            if (smokeTest)
                std::cerr << "VIEW3D_SHUTDOWN_EVENT_LOOP_DONE\n";
            if (smokeTest && result == 0)
                std::cout << "VIEW3D_GENERIC_PLUGIN_HOST_STARTUP_OK\n";
        }
    }
    // UI（含 ViewportContainer）先销毁并释放渲染表面，再关闭插件扩展和 Ogre 运行时。
    for (auto iterator = pluginObjects.rbegin(); iterator != pluginObjects.rend(); ++iterator) {
        if (auto *extension = dynamic_cast<RF::PluginEngine::IExtension *>(iterator->get())) {
            extension->onDestroy();
            runtime.getPluginContainer()->unpublishObjectsOwnedBy(iterator->get());
        }
    }
    pluginObjects.clear();
    if (auto *container = runtime.getPluginContainer()) {
        container->unpublishObject(View3D::ServiceIds::SceneTreeMenu,
                                   &sceneTreeMenuRegistry);
        container->unpublishObject(View3D::ServiceIds::DataOpen, &dataOpenRegistry);
        container->unpublishObject(View3D::ServiceIds::ViewportMenu, &viewportMenuRegistry);
    }
    if (smokeTest)
        std::cerr << "VIEW3D_SHUTDOWN_PLUGIN_OBJECTS_DONE\n";
    if (smokeTest)
        std::cerr << "VIEW3D_SHUTDOWN_UI_DONE\n";

    for (auto iterator = packages.crbegin(); iterator != packages.crend(); ++iterator)
        factories.unregisterPlugin(iterator->configuredId);
    if (smokeTest)
        std::cerr << "VIEW3D_SHUTDOWN_OWNERS_DEFERRED\n";
    View3DApplication::uninstallQtMessageLogBridge();
    return result;
}
