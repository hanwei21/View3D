#include "View3DPluginEntry.h"

#include "OsgbMetadata.h"
#include "OsgbRuntime.h"
#include "OsgbStreaming.h"

#include <osg/Notify>
#include <osgDB/Registry>

#include <IAppLog.h>
#include <IAppLogManager.h>
#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IGeoReference.h>
#include <IPlugin.h>
#include <IPluginContainer.h>
#include <IDataOpen.h>
#include <IRenderScene.h>
#include <IOsbLoader.h>
#include <ISceneLayerService.h>
#include <ISceneTreeMenu.h>
#include <IViewportLayout.h>
#include <TileCacheSettings.h>
#include <View3DObjectIds.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace RF::PluginEngine;

// osgDB 只在自己登记的目录里找读取器插件，显式注册避免依赖 PATH。
// 目录**相对 exe**：<exe目录>\plugins\osgPlugins-<版本>（随包部署就是这个布局），
// 或者直接 <exe目录>\osgPlugins-<版本>。exe 通常就在 bin 下，所以看起来像"从 bin 找"。
void configureOsgRuntime()
{
    osg::setNotifyLevel(osg::NotifySeverity::WARN);
    osgDB::Registry* registry = osgDB::Registry::instance();
    for (const QString& root : View3DOsb::osgPluginRoots()) {
        registry->getLibraryFilePathList().push_back(root.toStdString());
        std::cerr << "VIEW3D_OSGB_PLUGIN_ROOT dir=" << root.toStdString()
                  << " exists=" << (QDir(root).exists() ? 1 : 0) << std::endl;
    }
    const QStringList pluginDirs = View3DOsb::osgPluginDirs();
    if (pluginDirs.isEmpty()) {
        std::cerr << "VIEW3D_OSGB_PLUGIN_MISSING searched="
                  << View3DOsb::osgPluginSearchHint().toStdString() << std::endl;
    }
    for (const QString& path : pluginDirs) {
        registry->getLibraryFilePathList().push_back(path.toStdString());
        std::cerr << "VIEW3D_OSBG_PLUGIN_PATH path=" << path.toStdString() << std::endl;
    }
}

QString groupKeyOf(const QString& directory)
{
    return QStringLiteral("osgb:") + QFileInfo(QDir(directory).absolutePath()).fileName();
}

// Ribbon 命令与目录树右键菜单共用同一个选择入口。
QString chooseDatasetMetadata(QString* lastFile)
{
    const QString start = lastFile && !lastFile->isEmpty() ? *lastFile : QDir::homePath();
    const QString file = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择倾斜摄影 metadata.xml"), start,
        QStringLiteral("OSGB 元数据 (metadata.xml Metadata.xml);;所有文件 (*)"));
    if (lastFile && !file.isEmpty())
        *lastFile = file;
    return file;
}

// Ribbon 命令：选目录并交给倾斜摄影服务加载，宿主不需要知道数据插件的存在。
class OsgbCommand final : public ICommand {
  public:
    explicit OsgbCommand(const PluginCreationContext& context)
        : m_name(context.attributes.value(QStringLiteral("name"))),
          m_caption(context.attributes.value(QStringLiteral("caption"))),
          m_tooltip(context.attributes.value(QStringLiteral("hint")))
    {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("osgb"); }
    bool getChecked() const override { return false; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_tooltip; }
    bool getEnabled() const override { return loader() != nullptr; }

    void onClick() override
    {
        View3D::IOsbLoader* service = loader();
        if (!service)
            return;
        // 直接选 metadata.xml：数据集根目录可能在 Data 外面，也可能在 Data 里面。
        const QString file = chooseDatasetMetadata(&m_lastFile);
        if (file.isEmpty())
            return;
        const QString directory = QFileInfo(file).absolutePath();
        if (!service->loadDataset(directory)) {
            std::cerr << "VIEW3D_OSBG_OPEN_FAILED reason="
                      << service->lastError().toStdString() << std::endl;
        }
    }

    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    View3D::IOsbLoader* loader() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        return container
            ? container->objectAs<View3D::IOsbLoader>(View3D::ServiceIds::OsbLoader)
            : nullptr;
    }

    IApplication* m_application = nullptr;
    QString m_name;
    QString m_caption;
    QString m_tooltip;
    QString m_lastFile;
};

class OsgbExtension final : public IExtension,
                            public View3D::IOsbLoader,
                            public View3D::ISceneTreeMenuContributor,
                            public View3D::IDataOpener {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        m_application = application;
        configureOsgRuntime();
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        container->publishAs<View3D::IOsbLoader>(View3D::ServiceIds::OsbLoader, this, this);
        m_menuRegistry = container->objectAs<View3D::ISceneTreeMenuRegistry>(
            View3D::ServiceIds::SceneTreeMenu);
        if (m_menuRegistry)
            m_menuRegistry->addSceneTreeMenuContributor(this);
        m_dataOpenRegistry = container->objectAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen);
        if (m_dataOpenRegistry)
            m_dataOpenRegistry->addDataOpener(this);
        ensureGeoFrameListener();
        ensureGlobeListener();
        return true;
    }

    void onDestroy() override
    {
        if (m_globeToken != 0 && m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                if (auto* scene = container->objectAs<View3D::IRenderScene>(
                        View3D::ServiceIds::Scene))
                    scene->removeGlobeModeChangeListener(m_globeToken);
            }
            m_globeToken = 0;
        }
        if (m_geoFrameToken != 0 && m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                if (auto* reference = container->objectAs<View3D::IGeoReference>(
                        View3D::ServiceIds::GeoReference))
                    reference->removeGeoFrameChangeListener(m_geoFrameToken);
            }
            m_geoFrameToken = 0;
        }
        if (m_dataOpenRegistry) {
            m_dataOpenRegistry->removeDataOpener(this);
            m_dataOpenRegistry = nullptr;
        }
        if (m_menuRegistry) {
            m_menuRegistry->removeSceneTreeMenuContributor(this);
            m_menuRegistry = nullptr;
        }
        if (m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                container->unpublishObject(View3D::ServiceIds::OsbLoader,
                                           static_cast<View3D::IOsbLoader*>(this));
                auto* layers = container->objectAs<View3D::ISceneLayerService>(
                    View3D::ServiceIds::SceneLayers);
                for (const std::shared_ptr<Dataset>& dataset : std::as_const(m_datasets)) {
                    // 先停工作线程，再撤图层：图层移除会连带清掉场景里的瓦片。
                    dataset->streaming.reset();
                    if (layers && !dataset->layer.isNull())
                        layers->removeLayer(dataset->layer);
                }
            }
        }
        m_datasets.clear();
        m_application = nullptr;
    }

    QString lastError() const override { return m_lastError; }

    bool loadDataset(const QString& directory) override
    {
        return loadDatasetInto(directory, View3D::LayerId {});
    }

    bool loadDatasetInto(const QString& directory, const View3D::LayerId& parentGroup)
    {
        m_lastError.clear();
        const View3DOsb::DatasetDescriptor descriptor =
            View3DOsb::discoverDataset(directory);
        if (!descriptor.valid()) {
            m_lastError = descriptor.error;
            std::cerr << "VIEW3D_OSBG_DISCOVER_FAILED reason="
                      << descriptor.error.toStdString() << std::endl;
            return false;
        }
        std::cerr << "VIEW3D_OSBG_DISCOVER srs=" << descriptor.srs.toStdString()
                  << " origin=" << descriptor.origin[0] << "," << descriptor.origin[1] << ","
                  << descriptor.origin[2] << " roots=" << descriptor.rootTileFiles.size()
                  << " data=" << descriptor.dataDirectory.toStdString() << std::endl;

        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        auto* layers = container
            ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
            : nullptr;
        auto* layout = container
            ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
            : nullptr;
        if (!scene || !layers) {
            m_lastError = QStringLiteral("缺少 Scene 或 SceneLayers 服务");
            std::cerr << "VIEW3D_OSBG_NO_SERVICES scene=" << (scene ? 1 : 0)
                      << " layers=" << (layers ? 1 : 0) << std::endl;
            return false;
        }

        const QString groupKey = groupKeyOf(directory);
        if (m_datasets.contains(groupKey)) {
            m_lastError = QStringLiteral("该数据集已加载：%1").arg(groupKey);
            return false;
        }

        const QString datasetName = QFileInfo(QDir(directory).absolutePath()).fileName();
        auto* cacheConfig =
            container->objectAs<View3D::ITileCacheConfig>(View3D::ServiceIds::TileCache);
        const View3D::TileCacheSettings settings =
            cacheConfig ? cacheConfig->tileCacheSettings() : View3D::TileCacheSettings {};
        std::array<double, 3> originGeographic { {0.0, 0.0, 0.0} };
        const bool hasOriginGeographic =
            srsOriginGeographic(descriptor.srs, descriptor.origin, &originGeographic);
        auto* reference = container->objectAs<View3D::IGeoReference>(
            View3D::ServiceIds::GeoReference);
        // 世界原点首钉：相机局部↔ECEF 换算、坐标读数与 3D Tiles 叠加都需要地理基准。
        // 只钉一次；已由别的数据源钉住时按约定不搬动。
        if (hasOriginGeographic && reference && !reference->ready()) {
            reference->setOrigin(originGeographic[0], originGeographic[1],
                                 originGeographic[2]);
        }
        auto streaming = std::make_unique<View3DOsb::OsgbStreaming>(*scene, layout, descriptor,
                                                                   settings, reference);
        // 球面模式在加载时定死烘焙路径：与 3D Tiles 一致，切换只能重建会话。
        streaming->setGlobeMode(scene->globeModeEnabled());
        // metadata.xml 里的 SRS 就是成果的源参照系（多为 EPSG 投影坐标）；没写就留空。
        const View3D::LayerId layer = layers->addTileLayer(
            datasetName + QStringLiteral("（倾斜摄影）"), groupKey, parentGroup,
            crsDisplayText(descriptor.srs));
        if (layer.isNull()) {
            m_lastError = QStringLiteral("无法登记瓦片图层：%1").arg(groupKey);
            std::cerr << "VIEW3D_OSBG_LAYER_FAILED group=" << groupKey.toStdString()
                      << std::endl;
            return false;
        }
        if (!streaming->start()) {
            const QString detail = streaming->lastError();
            m_lastError = detail.isEmpty() ? QStringLiteral("根瓦片加载失败")
                                           : QStringLiteral("根瓦片加载失败：%1").arg(detail);
            std::cerr << "VIEW3D_OSBG_START_FAILED group=" << groupKey.toStdString()
                      << " reason=" << detail.toStdString() << std::endl;
            // 双击启动看不到 stderr：这条要进应用内日志，换机器排查才有依据。
            if (m_application) {
                if (auto* manager = m_application->getAppLogManager()) {
                    if (auto* logger = manager->getLogger(QStringLiteral("osgb"))) {
                        logger->error(QStringLiteral("倾斜摄影加载失败：%1").arg(directory),
                                      detail);
                    }
                }
            }
            layers->removeLayer(layer);
            return false;
        }
        applyReferenceFrame(layer, descriptor, streaming->appliedOrigin());
        const View3DOsb::OsgbStreaming& started = *streaming;
        std::cerr << "VIEW3D_OSBG_LOADED group=" << groupKey.toStdString()
                  << " resident=" << started.residentTiles()
                  << " mb=" << (started.retainedBytes() / (1024.0 * 1024.0))
                  << " preset=" << started.settings().preset.toStdString() << std::endl;

        auto dataset = std::make_shared<Dataset>();
        dataset->layer = layer;
        dataset->directory = directory;
        dataset->parentGroup = parentGroup;
        dataset->descriptor = descriptor;
        dataset->sourceCrs = descriptor.srs.trimmed();
        dataset->originGeographic = originGeographic;
        dataset->hasOriginGeographic = hasOriginGeographic;
        dataset->streaming = std::move(streaming);
        m_datasets.insert(groupKey, std::move(dataset));
        // 成果自己的朝向/位置取决于当前显示参照系与已钉住的世界原点，加载完立刻摆一次。
        ensureGeoFrameListener();
        ensureGlobeListener();
        applyDisplayPlacement();
        return true;
    }

    bool unloadDataset(const QString& directory) override
    {
        m_lastError.clear();
        const QString groupKey = groupKeyOf(directory);
        const auto found = m_datasets.find(groupKey);
        if (found == m_datasets.end()) {
            m_lastError = QStringLiteral("未加载该数据集：%1").arg(groupKey);
            return false;
        }
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* layers = container
            ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
            : nullptr;
        const std::shared_ptr<Dataset>& holder = found.value();
        holder->streaming.reset();
        if (layers && !holder->layer.isNull())
            layers->removeLayer(holder->layer);
        m_datasets.erase(found);
        std::cerr << "VIEW3D_OSBG_UNLOADED group=" << groupKey.toStdString() << std::endl;
        return true;
    }

    // 目录树右键菜单：数据集相关操作都留在这里，场景树插件不需要认识 OSGB。
    void appendSceneTreeMenu(const View3D::SceneTreeMenuTarget& target,
                             QList<View3D::SceneTreeMenuAction>* actions) override
    {
        if (!actions || target.kind != View3D::SceneTreeMenuTargetKind::Layer)
            return;
        const Dataset* dataset = datasetForLayer(target.primary);
        if (!dataset)
            return;

        const View3D::LayerId layerId = target.primary;
        const QString directory = dataset->directory;

        View3D::SceneTreeMenuAction locate;
        locate.text = QStringLiteral("定位到数据集");
        locate.icon = QStringLiteral("action-locate");
        locate.triggered = [this, layerId] { locateDataset(layerId); };
        actions->append(locate);

        View3D::SceneTreeMenuAction reload;
        reload.text = QStringLiteral("重新加载数据集");
        reload.icon = QStringLiteral("action-reload-dataset");
        reload.triggered = [this, directory] {
            if (!reloadDataset(directory))
                std::cerr << "VIEW3D_OSBG_RELOAD_FAILED reason="
                          << m_lastError.toStdString() << std::endl;
        };
        actions->append(reload);

        View3D::SceneTreeMenuAction unload;
        unload.text = QStringLiteral("卸载数据集");
        unload.icon = QStringLiteral("action-unload");
        unload.separatorBefore = true;
        unload.triggered = [this, directory] {
            if (!unloadDataset(directory))
                std::cerr << "VIEW3D_OSBG_UNLOAD_FAILED reason="
                          << m_lastError.toStdString() << std::endl;
        };
        actions->append(unload);
    }

    bool ownsSceneTreeLayer(const View3D::LayerId& layerId) const override
    {
        return datasetForLayer(layerId) != nullptr;
    }

    bool removeOwnedSceneTreeLayer(const View3D::LayerId& layerId) override
    {
        if (const Dataset* dataset = datasetForLayer(layerId))
            return unloadDataset(dataset->directory);
        return false;
    }

    // 通用“打开数据”入口：场景树统一弹文件对话框，倾斜摄影只负责识别和加载。
    QString displayName() const override { return QStringLiteral("倾斜摄影数据集"); }
    QString fileFilter() const override
    {
        return QStringLiteral("倾斜摄影数据集 (metadata.xml Metadata.xml)");
    }
    bool openData(const QString& path, const View3D::LayerId& parentGroup,
                  QString* error) override
    {
        const QFileInfo info(path);
        const QString directory =
            info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        if (!loadDatasetInto(directory, parentGroup)) {
            if (error)
                *error = m_lastError;
            return false;
        }
        if (error)
            error->clear();
        return true;
    }

  private:
    struct Dataset {
        View3D::LayerId layer;
        View3D::LayerId parentGroup;
        QString directory;
        std::unique_ptr<View3DOsb::OsgbStreaming> streaming;
        // 球面切换重建会话用：发现阶段的完整描述（SRS/SRSOrigin/根瓦片）。
        View3DOsb::DatasetDescriptor descriptor;
        // 投影跟随用：metadata 声明的源参照系、SRSOrigin 及其经纬度。
        // 没写 SRS 的成果不参与跟随（轴向未知，转了反而错）。
        QString sourceCrs;
        std::array<double, 3> originGeographic { {0.0, 0.0, 0.0} };
        bool hasOriginGeographic = false;
    };

    // SRSOrigin（源投影系平面米）→ 经纬度：显示参照系一变，成果该摆在哪儿就靠它。
    bool srsOriginGeographic(const QString& srs, const std::array<double, 3>& origin,
                             std::array<double, 3>* geographic) const
    {
        if (!geographic || srs.trimmed().isEmpty())
            return false;
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!reference)
            return false;
        *geographic = origin;
        return reference->transformToMain(srs.trimmed(), geographic->data(), 1);
    }

    // 把每个成果按当前显示参照系刚体摆好：源网格轴 → 显示网格轴（收敛角差 + 比例
    // 因子），再把 SRSOrigin 放到它在当前显示下的世界位置。投影切换与世界原点钉上
    // 都要重算，否则倾斜摄影会与逐点重放的矢量/栅格错位。
    void applyDisplayPlacement()
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!scene || !reference)
            return;
        // 球面模式按逐顶点 ECEF 烘焙，平面刚体摆放（yaw/scale/originLocal）不适用。
        if (scene->globeModeEnabled())
            return;
        for (auto it = m_datasets.cbegin(); it != m_datasets.cend(); ++it) {
            const std::shared_ptr<Dataset>& dataset = it.value();
            if (!dataset || !dataset->hasOriginGeographic || dataset->sourceCrs.isEmpty())
                continue;
            View3D::IGeoReference::PlanePlacement placement;
            if (!reference->planePlacement(dataset->sourceCrs,
                                           dataset->originGeographic[0],
                                           dataset->originGeographic[1],
                                           dataset->originGeographic[2], &placement))
                continue;
            const QString groupKey = it.key();
            scene->setTileGroupPlaneTransform(groupKey, placement.yawDegrees,
                                              placement.horizontalScale,
                                              placement.originLocal);
            std::cerr << "VIEW3D_OSBG_PLACEMENT group=" << groupKey.toStdString()
                      << " yaw=" << placement.yawDegrees
                      << " scale=" << placement.horizontalScale
                      << " offset=" << placement.originLocal[0] << ","
                      << placement.originLocal[1] << "," << placement.originLocal[2] << std::endl;
        }
    }

    // crs 插件可能比本插件晚就绪，所以加载成果时再补一次订阅。
    void ensureGeoFrameListener()
    {
        if (m_geoFrameToken != 0)
            return;
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!reference)
            return;
        m_geoFrameToken = reference->addGeoFrameChangeListener(
            [this](View3D::IGeoReference::GeoFrameChange, const QString& displayCrs) {
                applyDisplayPlacement();
                std::cerr << "VIEW3D_OSBG_GEOFRAME changed display=" << displayCrs.toStdString()
                          << std::endl;
            });
    }

    // 球面模式切换：烘焙路径在会话创建时定死，只能整体重建会话（与 3D Tiles 一致）。
    // 相机由布局编排层换算；keepCameraOnRestart 阻止数据集重新框取景。
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
            auto* container = m_application ? m_application->getPluginContainer() : nullptr;
            auto* scene = container
                ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
                : nullptr;
            auto* layout = container
                ? container->objectAs<View3D::IViewportLayout>(
                      View3D::ServiceIds::ViewportLayout)
                : nullptr;
            auto* reference = container
                ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
                : nullptr;
            auto* cacheConfig = container
                ? container->objectAs<View3D::ITileCacheConfig>(View3D::ServiceIds::TileCache)
                : nullptr;
            if (!scene || !layout)
                return;
            const View3D::TileCacheSettings settings =
                cacheConfig ? cacheConfig->tileCacheSettings() : View3D::TileCacheSettings {};
            for (auto it = m_datasets.begin(); it != m_datasets.end(); ++it) {
                const std::shared_ptr<Dataset>& dataset = it.value();
                if (dataset->streaming) {
                    // 先停线程（alive=false 后排队的主线程回调不再落户），再撤瓦片。
                    dataset->streaming->stop();
                    dataset->streaming->detachFromScene();
                }
                dataset->streaming.reset();
                auto streaming = std::make_unique<View3DOsb::OsgbStreaming>(
                    *scene, layout, dataset->descriptor, settings, reference);
                streaming->setGlobeMode(enabled);
                streaming->keepCameraOnRestart();
                if (!streaming->start()) {
                    std::cerr << "VIEW3D_OSBG_GLOBE_RESTART_FAILED group="
                              << it.key().toStdString()
                              << " reason=" << streaming->lastError().toStdString()
                              << std::endl;
                } else {
                    dataset->streaming = std::move(streaming);
                }
                // 参照系描述随模式切换：球面不再展示"源→场景局部"换算矩阵。
                applyReferenceFrame(dataset->layer, dataset->descriptor,
                                    dataset->streaming ? dataset->streaming->appliedOrigin()
                                                       : dataset->descriptor.origin);
            }
            if (!enabled)
                applyDisplayPlacement();
            std::cerr << "VIEW3D_OSBG_GLOBE mode=" << (enabled ? 1 : 0)
                      << " groups=" << m_datasets.size() << std::endl;
        });
    }

    // 参照系展示文本：EPSG 码后补 PROJ 的官方名称（如 "CGCS2000 / 3-degree …"），
    // 属性面板读起来才知道是哪个坐标系；crs 服务缺失时只留代码。
    QString crsDisplayText(const QString& crs) const
    {
        const QString trimmed = crs.trimmed();
        if (trimmed.isEmpty())
            return {};
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        View3D::IGeoReference::CrsInfo info;
        if (reference && reference->crsInfo(trimmed, &info) && !info.name.isEmpty())
            return QStringLiteral("%1（%2）").arg(trimmed, info.name);
        return trimmed;
    }

    // 参照系描述：倾斜摄影的源坐标是成果声明的投影坐标（x=东, y=北, z=上，米），
    // 渲染前先减 SRSOrigin 再做 (E,N,U)→(E,U,-N) 的轴交换；两者合成一个仿射矩阵
    // 回填给图层属性。经纬度由 SRSOrigin 经 crs 服务投影反算，成果没写就留空。
    void applyReferenceFrame(const View3D::LayerId& layer,
                             const View3DOsb::DatasetDescriptor& descriptor,
                             const std::array<double, 3>& appliedOrigin)
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* layers = container
            ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
            : nullptr;
        if (!layers || layer.isNull())
            return;
        View3D::ILayer::ReferenceFrame frame;
        // 球面模式逐顶点按 ECEF 烘焙，没有"源→场景局部"的换算矩阵，面板不展示。
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        frame.hasMatrix = !(scene && scene->globeModeEnabled());
        if (frame.hasMatrix) {
            // 行主序 4×4：scene = R·(source − origin)，R 把 (x,y,z) 映射成 (x,z,−y)。
            frame.matrix = {1.0, 0.0, 0.0, -appliedOrigin[0],
                            0.0, 0.0, 1.0, -appliedOrigin[2],
                            0.0, -1.0, 0.0, appliedOrigin[1],
                            0.0, 0.0, 0.0, 1.0};
        }
        const std::array<double, 3>& datasetOrigin = descriptor.origin;
        if (datasetOrigin[0] != 0.0 || datasetOrigin[1] != 0.0 || datasetOrigin[2] != 0.0) {
            frame.origin = QStringLiteral("SRSOrigin %1, %2, %3 m")
                               .arg(datasetOrigin[0], 0, 'f', 3)
                               .arg(datasetOrigin[1], 0, 'f', 3)
                               .arg(datasetOrigin[2], 0, 'f', 3);
            auto* reference = container
                ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
                : nullptr;
            std::vector<double> xyz {datasetOrigin[0], datasetOrigin[1], datasetOrigin[2]};
            if (reference && !descriptor.srs.trimmed().isEmpty() &&
                reference->transformToMain(descriptor.srs, xyz.data(), 1)) {
                frame.position = QStringLiteral("%1°, %2°, 椭球高 %3 m")
                                     .arg(xyz[0], 0, 'f', 8)
                                     .arg(xyz[1], 0, 'f', 8)
                                     .arg(xyz[2], 0, 'f', 3);
            }
        }
        layers->setLayerReferenceFrame(layer, frame);
    }

    const Dataset* datasetForLayer(const View3D::LayerId& layerId) const
    {
        for (auto it = m_datasets.cbegin(); it != m_datasets.cend(); ++it) {
            if (it.value()->layer == layerId)
                return it.value().get();
        }
        return nullptr;
    }

    bool locateDataset(const View3D::LayerId& layerId)
    {
        const Dataset* dataset = datasetForLayer(layerId);
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* layout = container
            ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
            : nullptr;
        if (!dataset || !dataset->streaming || !layout)
            return false;
        const auto focusOne = [&](View3D::IRenderSurface* view) {
            return view && view->focusTileGroup(dataset->streaming->groupKey());
        };
        const QList<View3D::IRenderSurface*> views = layout->views();
        if (views.isEmpty())
            return focusOne(layout->activeView());
        bool located = false;
        for (View3D::IRenderSurface* view : views)
            located = focusOne(view) || located;
        return located;
    }

    bool reloadDataset(const QString& directory)
    {
        const auto found = m_datasets.find(groupKeyOf(directory));
        if (found == m_datasets.end()) {
            m_lastError = QStringLiteral("未加载该数据集：%1").arg(groupKeyOf(directory));
            return false;
        }
        const View3D::LayerId parentGroup = found.value()->parentGroup;
        if (!unloadDataset(directory))
            return false;
        return loadDatasetInto(directory, parentGroup);
    }

    IApplication* m_application = nullptr;
    View3D::ISceneTreeMenuRegistry* m_menuRegistry = nullptr;
    View3D::IDataOpenRegistry* m_dataOpenRegistry = nullptr;
    QHash<QString, std::shared_ptr<Dataset>> m_datasets;
    std::uint64_t m_geoFrameToken = 0;
    std::uint64_t m_globeToken = 0;
    QString m_lastError;
};

void registerOsgbFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.osgb"), QStringLiteral("extension.osgb_loader"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new OsgbExtension(); });
    for (const QString& type : {QStringLiteral("command"), QStringLiteral("checkcommand")}) {
        registry->registerCreator(
            QStringLiteral("plugin.view3d.osgb"), QStringLiteral("factory.create_command"), type,
            [](const PluginCreationContext& context) -> IPlugin* {
                return new OsgbCommand(context);
            });
    }
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.osgb", registerOsgbFactories(registry);)
