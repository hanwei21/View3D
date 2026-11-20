#include "View3DPluginEntry.h"

#include "ModelImportSession.h"
#include "ModelOpenFlow.h"
#include "ModelReadTypes.h"

#ifdef VIEW3D_MODEL_HAS_OSG
#include <osg/Notify>
#include <osgDB/Registry>

#include <QCoreApplication>
#include <QDir>
#endif

#include <IDataOpen.h>
#include <IApplication.h>
#include <IExtension.h>
#include <IPluginContainer.h>
#include <IGeoReference.h>
#include <IRenderScene.h>
#include <View3DObjectIds.h>

#include <QFileInfo>
#include <QMainWindow>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace RF::PluginEngine;

#ifdef VIEW3D_MODEL_HAS_OSG
// osgDB 只在自己登记的目录里找读取器插件。倾斜摄影插件启动时已经登记过同一批路径，
// 这里在它缺席（缺依赖或被禁用）时补上，保证 .osgt/.ive/OpenFlight 仍能打开。
void configureOsgPluginPath()
{
    osgDB::Registry* registry = osgDB::Registry::instance();
    if (!registry->getLibraryFilePathList().empty())
        return;
    osg::setNotifyLevel(osg::NotifySeverity::WARN);
    const QDir appDir(QCoreApplication::applicationDirPath());
    registry->getLibraryFilePathList().push_back(appDir.absolutePath().toStdString());
    QDir pluginsDir = appDir;
    pluginsDir.cd(QStringLiteral("plugins"));
    const QDir searchDirs[2] = {pluginsDir, appDir};
    for (const QDir& dir : searchDirs) {
        const QStringList found =
            dir.entryList({QStringLiteral("osgPlugins-*")}, QDir::Dirs | QDir::NoDotAndDotDot,
                          QDir::Name);
        for (const QString& entry : found) {
            registry->getLibraryFilePathList().push_back(
                dir.absoluteFilePath(entry).toStdString());
        }
    }
}
#endif

class ModelExtension;

// 一个类别一个打开器：场景树的「打开数据」把每个打开器的过滤器拼成一行，
// 于是模型/点云/栅格/矢量在同一个对话框里各占一行，与旧版 ribbon 的
// 「打开模型」文件对话框一致。
class ModelOpener final : public View3D::IDataOpener {
  public:
    ModelOpener(View3DModel::ImportFormatKind kind, ModelExtension* owner)
        : m_kind(kind), m_owner(owner)
    {
    }

    QString displayName() const override;
    QString fileFilter() const override;
    bool openData(const QString& path, const View3D::LayerId& parentGroup,
                  QString* error) override;

  private:
    View3DModel::ImportFormatKind m_kind = View3DModel::ImportFormatKind::Model;
    ModelExtension* m_owner = nullptr;
};

class ModelExtension final : public IExtension {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        m_application = application;
#ifdef VIEW3D_MODEL_HAS_OSG
        configureOsgPluginPath();
#endif
        m_session = std::make_unique<View3DModel::ModelImportSession>(application);
        if (!m_session->start()) {
            m_session.reset();
            // 图层服务还没发布（本插件比 SceneView 先创建）：返回 false 让宿主按
            // 扩展重试机制再来一次，不是错误。
            std::cerr << "VIEW3D_MODEL_WAIT_SERVICES" << std::endl;
            return false;
        }
        m_openRegistry = container->objectAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen);
        if (!m_openRegistry) {
            m_session->stop();
            m_session.reset();
            std::cerr << "VIEW3D_MODEL_NO_REGISTRY" << std::endl;
            return false;
        }
        using View3DModel::ImportFormatKind;
        for (const ImportFormatKind kind :
             {ImportFormatKind::Model, ImportFormatKind::PointCloud, ImportFormatKind::Raster,
              ImportFormatKind::Vector}) {
            auto opener = std::make_unique<ModelOpener>(kind, this);
            // 该类别的读取器没编进来（缺依赖）时过滤器为空，不注册空入口。
            if (opener->fileFilter().isEmpty())
                continue;
            m_openRegistry->addDataOpener(opener.get());
            m_openers.emplace_back(std::move(opener));
        }
        std::cerr << "VIEW3D_MODEL_OPENERS count=" << m_openers.size() << " readers="
                  << View3DModel::modelFormatReaders().all().size() << std::endl;
        // 显示参照系切换 → 已导入的地理图层按新映射原地重放。
        ensureGeoFrameListener();
        // 球面模式切换 → 模型图层重烘焙到 ECEF / 换回平面几何。
        ensureGlobeListener();
        return true;
    }

    void onDestroy() override
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (reference && m_displayToken != 0)
            reference->removeGeoFrameChangeListener(m_displayToken);
        m_displayToken = 0;
        if (container && m_globeToken != 0) {
            if (auto* scene = container->objectAs<View3D::IRenderScene>(
                    View3D::ServiceIds::Scene))
                scene->removeGlobeModeChangeListener(m_globeToken);
        }
        m_globeToken = 0;
        if (m_openRegistry) {
            for (const std::unique_ptr<ModelOpener>& opener : m_openers)
                m_openRegistry->removeDataOpener(opener.get());
        }
        m_openers.clear();
        m_openRegistry = nullptr;
        if (m_session) {
            m_session->stop();
            m_session.reset();
        }
        m_application = nullptr;
    }

    // crs 服务可能比本插件晚就绪（扩展创建轮次不保证顺序），所以每次打开数据都补一次
    // 订阅，否则切换投影时这一轮导入的图层永远不会重放。
    void ensureGeoFrameListener()
    {
        if (m_displayToken != 0)
            return;
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!reference)
            return;
        m_displayToken = reference->addGeoFrameChangeListener(
            [this](View3D::IGeoReference::GeoFrameChange change, const QString& displayCrs) {
                // 原点钉上时不用重放：导入的顶点本来就是按当时已定的原点算的。
                if (change != View3D::IGeoReference::GeoFrameChange::DisplayCrs)
                    return;
                QString error;
                // 没有可重放的图层只记一行日志：切投影本身是合法操作，不该弹错误框。
                if (!m_session || !m_session->submitReprojection(displayCrs, &error)) {
                    std::cerr << "VIEW3D_MODEL_REPLAY_NONE crs=" << displayCrs.toStdString()
                              << " reason=" << error.toStdString() << std::endl;
                }
            });
    }

    // 球面模式切换：模型图层在工作线程重烘焙到 ECEF（有源地理坐标的按经纬度，
    // 没有的钉在 (0°,0°) 地表）；退出时恢复平面几何。
    void ensureGlobeListener()
    {
        if (m_globeToken != 0)
            return;
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        if (!scene)
            return;
        m_globeToken = scene->addGlobeModeChangeListener([this](bool enabled) {
            QString error;
            if (!m_session || !m_session->submitGlobeRebuild(enabled, &error)) {
                std::cerr << "VIEW3D_MODEL_GLOBE_NONE mode=" << (enabled ? 1 : 0)
                          << " reason=" << error.toStdString() << std::endl;
            }
        });
    }

    View3DModel::ModelImportSession* session() const
    {
        return m_session.get();
    }

    QWidget* dialogParent() const
    {
        return m_application ? m_application->getMainWindow() : nullptr;
    }

    View3DModel::ModelImportOptions options() const
    {
        return m_options;
    }

    void setOptions(const View3DModel::ModelImportOptions& options)
    {
        m_options = options;
    }

  private:
    IApplication* m_application = nullptr;
    View3D::IDataOpenRegistry* m_openRegistry = nullptr;
    std::unique_ptr<View3DModel::ModelImportSession> m_session;
    std::vector<std::unique_ptr<ModelOpener>> m_openers;
    View3DModel::ModelImportOptions m_options;
    std::uint64_t m_displayToken = 0;
    std::uint64_t m_globeToken = 0;
};

QString ModelOpener::displayName() const
{
    return View3DModel::displayNameForCategory(m_kind);
}

QString ModelOpener::fileFilter() const
{
    return View3DModel::fileFilterForCategory(m_kind);
}

bool ModelOpener::openData(const QString& path, const View3D::LayerId& parentGroup,
                           QString* error)
{
    ModelExtension* owner = m_owner;
    if (!owner || !owner->session())
        return false;
    owner->ensureGeoFrameListener();
    const QString readerId = View3DModel::selectReaderId(path, m_kind);
    if (readerId.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法按「%1」读取：%2").arg(displayName(), QFileInfo(path).fileName());
        return false;
    }

    View3DModel::ModelImportOptions options = owner->options();
    if (!View3DModel::askImportOptions(owner->dialogParent(), m_kind, &options)) {
        // 用户取消不是失败：返回已处理，否则场景树会再弹一个「无法打开数据」。
        std::cerr << "VIEW3D_MODEL_OPEN_CANCELLED path=" << QFileInfo(path).fileName().toStdString()
                  << std::endl;
        if (error)
            error->clear();
        return true;
    }
    owner->setOptions(options);
    if (error)
        error->clear();
    return owner->session()->submitWithReader(path, readerId, options, parentGroup, error);
}

void registerModelFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.model"), QStringLiteral("extension.model_import"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new ModelExtension(); });
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.model", registerModelFactories(registry);)
