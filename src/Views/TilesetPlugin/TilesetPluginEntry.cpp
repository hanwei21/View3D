#include "View3DPluginEntry.h"

#include "TilesetStreaming.h"

#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IPlugin.h>
#include <IGeoReference.h>
#include <IPluginContainer.h>
#include <IDataOpen.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <ISceneLayerService.h>
#include <ISceneTreeMenu.h>
#include <ITilesetLoader.h>
#include <IViewportCamera.h>
#include <IViewportLayout.h>
#include <TileCacheSettings.h>
#include <View3DObjectIds.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using namespace RF::PluginEngine;

QString resolveManifest(const QString& path)
{
    const QFileInfo info(path);
    if (info.isDir())
        return QDir(info.absoluteFilePath()).filePath(QStringLiteral("tileset.json"));
    return info.absoluteFilePath();
}

QString groupKeyOf(const QString& manifest)
{
    return QStringLiteral("tileset:") + QDir::cleanPath(QFileInfo(manifest).absoluteFilePath());
}

QString datasetNameOf(const QString& manifest)
{
    return QFileInfo(QFileInfo(manifest).absolutePath()).fileName();
}

bool validateManifest(const QString& manifest, QString* error)
{
    const QFileInfo info(manifest);
    if (!info.exists() || !info.isFile()) {
        if (error)
            *error = QStringLiteral("找不到 tileset.json：%1").arg(manifest);
        return false;
    }
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 tileset.json：%1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("tileset.json 无效：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    const QString version =
        root.value(QStringLiteral("asset")).toObject().value(QStringLiteral("version")).toString();
    if (!version.startsWith(QStringLiteral("1."))) {
        if (error)
            *error = QStringLiteral("不支持的 3D Tiles 版本：%1").arg(version);
        return false;
    }
    if (!root.value(QStringLiteral("root")).isObject()) {
        if (error)
            *error = QStringLiteral("tileset.json 缺少 root 瓦片");
        return false;
    }
    return true;
}

QString chooseManifest(QString* lastFile)
{
    const QString start =
        lastFile && !lastFile->isEmpty() ? *lastFile : QDir::homePath();
    const QString file = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择 3D Tiles 数据集（tileset.json）"), start,
        QStringLiteral("3D Tiles 数据集 (tileset.json Tileset.json);;所有文件 (*)"));
    if (lastFile && !file.isEmpty())
        *lastFile = file;
    return file;
}

class TilesetCommand final : public ICommand {
  public:
    explicit TilesetCommand(const PluginCreationContext& context)
        : m_name(context.attributes.value(QStringLiteral("name"))),
          m_caption(context.attributes.value(QStringLiteral("caption"))),
          m_tooltip(context.attributes.value(QStringLiteral("hint")))
    {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("tileset"); }
    bool getChecked() const override { return false; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_tooltip; }
    bool getEnabled() const override { return loader() != nullptr; }

    void onClick() override
    {
        View3D::ITilesetLoader* service = loader();
        if (!service)
            return;
        const QString file = chooseManifest(&m_lastFile);
        if (file.isEmpty())
            return;
        if (!service->loadTileset(file)) {
            std::cerr << "VIEW3D_TILES_OPEN_FAILED reason="
                      << service->lastError().toStdString() << std::endl;
        }
    }

    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    View3D::ITilesetLoader* loader() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        return container
            ? container->objectAs<View3D::ITilesetLoader>(View3D::ServiceIds::TilesetLoader)
            : nullptr;
    }

    IApplication* m_application = nullptr;
    QString m_name;
    QString m_caption;
    QString m_tooltip;
    QString m_lastFile;
};

class TilesetExtension final : public IExtension,
                               public View3D::ITilesetLoader,
                               public View3D::ISceneTreeMenuContributor,
                               public View3D::IDataOpener {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        m_application = application;
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        container->publishAs<View3D::ITilesetLoader>(View3D::ServiceIds::TilesetLoader,
                                                      this, this);
        m_menuRegistry = container->objectAs<View3D::ISceneTreeMenuRegistry>(
            View3D::ServiceIds::SceneTreeMenu);
        if (m_menuRegistry)
            m_menuRegistry->addSceneTreeMenuContributor(this);
        m_dataOpenRegistry = container->objectAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen);
        if (m_dataOpenRegistry)
            m_dataOpenRegistry->addDataOpener(this);
        m_frame = makeWorldFrame();
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
                container->unpublishObject(View3D::ServiceIds::TilesetLoader,
                                           static_cast<View3D::ITilesetLoader*>(this));
                auto* layers = container->objectAs<View3D::ISceneLayerService>(
                    View3D::ServiceIds::SceneLayers);
                for (const std::shared_ptr<Dataset>& dataset : std::as_const(m_datasets)) {
                    try {
                        if (dataset->streaming)
                            dataset->streaming->stop();
                        if (layers && !dataset->layer.isNull())
                            layers->removeLayer(dataset->layer);
                    } catch (...) {
                        std::cerr << "VIEW3D_TILES_DESTROY_EXCEPTION group="
                                  << dataset->manifest.toStdString() << std::endl;
                    }
                }
            }
        }
        m_datasets.clear();
        m_frame.reset();
        m_application = nullptr;
    }

    QString lastError() const override { return m_lastError; }

    bool loadTileset(const QString& tilesetJson) override
    {
        return loadTilesetInto(tilesetJson, View3D::LayerId {});
    }

    bool loadTilesetInto(const QString& path, const View3D::LayerId& parentGroup)
    {
        try {
            return loadTilesetIntoImpl(path, parentGroup);
        } catch (const std::exception& error) {
            m_lastError = QStringLiteral("加载 3D Tiles 数据异常：%1")
                              .arg(QString::fromUtf8(error.what()));
        } catch (...) {
            m_lastError = QStringLiteral("加载 3D Tiles 数据异常");
        }
        std::cerr << "VIEW3D_TILES_LOAD_EXCEPTION reason="
                  << m_lastError.toStdString() << std::endl;
        return false;
    }

    bool loadTilesetIntoImpl(const QString& path, const View3D::LayerId& parentGroup)
    {
        m_lastError.clear();
        const QString manifest = resolveManifest(path);
        if (!validateManifest(manifest, &m_lastError)) {
            std::cerr << "VIEW3D_TILES_DISCOVER_FAILED reason="
                      << m_lastError.toStdString() << std::endl;
            return false;
        }
        const QString groupKey = groupKeyOf(manifest);
        if (m_datasets.contains(groupKey)) {
            m_lastError = QStringLiteral("该数据集已加载：%1").arg(groupKey);
            return false;
        }

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
            std::cerr << "VIEW3D_TILES_NO_SERVICES scene=" << (scene ? 1 : 0)
                      << " layers=" << (layers ? 1 : 0) << std::endl;
            return false;
        }
        // 所有数据集都卸载后必须换一个新的世界系：否则下一个数据集的
        // 局部原点/ENU 朝向仍沿用上一份数据，focus 的机位方向和导航枢轴
        // 会与"只加载这一份"时不一致，旋转/移动也会更早碰到俯仰等限制。
        if ((m_datasets.isEmpty() || !m_frame) &&
            !qEnvironmentVariableIsSet("VIEW3D_TILES_KEEP_FRAME"))
            m_frame = makeWorldFrame();

        auto* cacheConfig =
            container->objectAs<View3D::ITileCacheConfig>(View3D::ServiceIds::TileCache);
        const View3D::TileCacheSettings settings =
            cacheConfig ? cacheConfig->tileCacheSettings() : View3D::TileCacheSettings {};
        const QString displayName = datasetNameOf(manifest);
        auto streaming = std::make_unique<View3DTiles::TilesetStreaming>(
            *scene, layout, m_frame, manifest, groupKey, displayName, settings);
        // 3D Tiles 规范固定用 WGS84 地心笛卡尔坐标（ECEF，米，EPSG:4978）：参照系写死在
        // 图层上，矩阵等世界系算出来后再回填——局部轴是 ENU 旋转过来的 (E,U,-N)。
        const View3D::LayerId layer = layers->addTileLayer(
            displayName + QStringLiteral("（3D Tiles）"), groupKey, parentGroup,
            QStringLiteral("EPSG:4978（WGS84 地心笛卡尔）"));
        if (layer.isNull()) {
            m_lastError = QStringLiteral("无法登记瓦片图层：%1").arg(groupKey);
            std::cerr << "VIEW3D_TILES_LAYER_FAILED group=" << groupKey.toStdString()
                      << std::endl;
            return false;
        }
        streaming->setWorldFrameCallback(
            [this, layer](const View3DTiles::TilesetWorldFrame::Snapshot& snapshot) {
                applyWorldFrame(layer, snapshot);
            });
        // 球面模式下新加载的数据集直接走球面烘焙，根取景也写 ECEF 相机。
        streaming->setGlobeMode(scene->globeModeEnabled());
        if (!streaming->start()) {
            m_lastError = streaming->lastError();
            if (m_lastError.isEmpty())
                m_lastError = QStringLiteral("Tileset 会话启动失败");
            layers->removeLayer(layer);
            std::cerr << "VIEW3D_TILES_START_FAILED group=" << groupKey.toStdString()
                      << " reason=" << m_lastError.toStdString() << std::endl;
            return false;
        }

        auto dataset = std::make_shared<Dataset>();
        dataset->layer = layer;
        dataset->parentGroup = parentGroup;
        dataset->manifest = manifest;
        dataset->streaming = std::move(streaming);
        m_datasets.insert(groupKey, std::move(dataset));
        ensureGeoFrameListener();
        ensureGlobeListener();
        applyDisplayPlacement();
        std::cerr << "VIEW3D_TILES_LOADED group=" << groupKey.toStdString()
                  << " name=" << displayName.toStdString() << std::endl;
        return true;
    }

    bool unloadTileset(const QString& tilesetJson) override
    {
        try {
            return unloadTilesetImpl(tilesetJson);
        } catch (const std::exception& error) {
            m_lastError = QStringLiteral("卸载 3D Tiles 数据异常：%1")
                              .arg(QString::fromUtf8(error.what()));
        } catch (...) {
            m_lastError = QStringLiteral("卸载 3D Tiles 数据异常");
        }
        std::cerr << "VIEW3D_TILES_UNLOAD_EXCEPTION reason="
                  << m_lastError.toStdString() << std::endl;
        return false;
    }

    bool unloadTilesetImpl(const QString& tilesetJson)
    {
        m_lastError.clear();
        const QString manifest = resolveManifest(tilesetJson);
        const QString groupKey = groupKeyOf(manifest);
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
        if (holder->streaming)
            holder->streaming->stop();
        if (layers && !holder->layer.isNull())
            layers->removeLayer(holder->layer);
        m_datasets.erase(found);
        if (m_datasets.isEmpty() &&
            !qEnvironmentVariableIsSet("VIEW3D_TILES_KEEP_FRAME")) {
            // 旧会话可能还有异步任务持有旧 frame；换对象而不是原地 reset，
            // 让新数据集拿到干净的原点，旧任务继续用它们自己的快照。
            m_frame = makeWorldFrame();
        }
        std::cerr << "VIEW3D_TILES_UNLOADED group=" << groupKey.toStdString() << std::endl;
        return true;
    }

    void appendSceneTreeMenu(const View3D::SceneTreeMenuTarget& target,
                             QList<View3D::SceneTreeMenuAction>* actions) override
    {
        if (!actions || target.kind != View3D::SceneTreeMenuTargetKind::Layer)
            return;
        const Dataset* dataset = datasetForLayer(target.primary);
        if (!dataset)
            return;

        const View3D::LayerId layerId = target.primary;
        const QString manifest = dataset->manifest;

        View3D::SceneTreeMenuAction locate;
        locate.text = QStringLiteral("定位到数据集");
        locate.icon = QStringLiteral("action-locate");
        locate.triggered = [this, layerId] { locateDataset(layerId); };
        actions->append(locate);

        View3D::SceneTreeMenuAction reload;
        reload.text = QStringLiteral("重新加载数据集");
        reload.icon = QStringLiteral("action-reload-dataset");
        reload.triggered = [this, manifest] {
            if (!reloadDataset(manifest))
                std::cerr << "VIEW3D_TILES_RELOAD_FAILED reason="
                          << m_lastError.toStdString() << std::endl;
        };
        actions->append(reload);

        View3D::SceneTreeMenuAction unload;
        unload.text = QStringLiteral("卸载数据集");
        unload.icon = QStringLiteral("action-unload");
        unload.separatorBefore = true;
        unload.triggered = [this, manifest] {
            if (!unloadTileset(manifest))
                std::cerr << "VIEW3D_TILES_UNLOAD_FAILED reason="
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
            return unloadTileset(dataset->manifest);
        return false;
    }

    QString displayName() const override { return QStringLiteral("3D Tiles 数据集"); }
    QString fileFilter() const override
    {
        return QStringLiteral("3D Tiles 数据集 (tileset.json Tileset.json)");
    }
    bool openData(const QString& path, const View3D::LayerId& parentGroup,
                  QString* error) override
    {
        if (!loadTilesetInto(path, parentGroup)) {
            if (error)
                *error = m_lastError;
            return false;
        }
        if (error)
            error->clear();
        return true;
    }

  private:
    // 瓦片局部系是世界原点处的真北 ENU（eastNorthUpToFixedFrame），显示参照系换成
    // 投影系后要跟网格北一起转：这里只给整组加「绕 up 旋转 + 水平等比缩放」，
    // 瓦片几何与流式缓都不动，所以切回经纬度显示时又能原样回来。
    void applyDisplayPlacement()
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        auto* reference = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!scene || !reference || m_datasets.isEmpty())
            return;
        // 球面锚定的瓦片不吃组旋转/缩放：显示投影的摆放只在平面模式生效。
        if (scene->globeModeEnabled())
            return;
        double originLon = 0.0;
        double originLat = 0.0;
        double originHeight = 0.0;
        if (!reference->origin(&originLon, &originLat, &originHeight))
            return;   // 还没有世界原点，ENU 框架本身也没定，保持现状
        View3D::IGeoReference::PlanePlacement placement;
        if (!reference->planePlacement(reference->mainCrs(), originLon, originLat, originHeight,
                                       &placement))
            return;
        for (auto it = m_datasets.cbegin(); it != m_datasets.cend(); ++it) {
            scene->setTileGroupPlaneTransform(it.key(), placement.yawDegrees,
                                             placement.horizontalScale);
        }
        std::cerr << "VIEW3D_TILES_PLACEMENT groups=" << m_datasets.size()
                  << " yaw=" << placement.yawDegrees
                  << " scale=" << placement.horizontalScale << std::endl;
    }

    // crs 插件可能比本插件晚就绪，加载数据集时再补一次订阅。
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
                std::cerr << "VIEW3D_TILES_GEOFRAME display=" << displayCrs.toStdString()
                          << std::endl;
            });
    }

    // 球面模式切换：烘焙路径在会话创建时定死，只能 stop→set→start 整体重启。
    // 相机由 SceneView 编排层换算，keepCameraOnRestart 阻止根取景覆盖它。
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
            for (auto it = m_datasets.begin(); it != m_datasets.end(); ++it) {
                const std::shared_ptr<Dataset>& dataset = it.value();
                if (!dataset->streaming)
                    continue;
                dataset->streaming->stop();
                dataset->streaming->setGlobeMode(enabled);
                dataset->streaming->keepCameraOnRestart();
                if (!dataset->streaming->start())
                    std::cerr << "VIEW3D_TILES_GLOBE_RESTART_FAILED group="
                              << it.key().toStdString() << std::endl;
            }
            // 参照系描述随模式切换：球面不展示局部换算矩阵，退出时恢复。
            if (m_frame) {
                const View3DTiles::TilesetWorldFrame::Snapshot snapshot =
                    m_frame->snapshot();
                if (snapshot.ready) {
                    for (auto it = m_datasets.begin(); it != m_datasets.end(); ++it)
                        applyWorldFrame(it.value()->layer, snapshot);
                }
            }
            if (!enabled)
                applyDisplayPlacement();
            std::cerr << "VIEW3D_TILES_GLOBE mode=" << (enabled ? 1 : 0)
                      << " groups=" << m_datasets.size() << std::endl;
        });
    }

    struct Dataset {
        View3D::LayerId layer;
        View3D::LayerId parentGroup;
        QString manifest;
        std::unique_ptr<View3DTiles::TilesetStreaming> streaming;
    };

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
        const QString groupKey = dataset->streaming->groupKey();
        const auto focusOne = [&](View3D::IRenderSurface* view) {
            return view && view->focusTileGroup(groupKey);
        };
        const QList<View3D::IRenderSurface*> views = layout->views();
        if (views.isEmpty())
            return focusOne(layout->activeView());
        bool located = false;
        for (View3D::IRenderSurface* view : views)
            located = focusOne(view) || located;
        return located;
    }

    bool reloadDataset(const QString& manifest)
    {
        const auto found = m_datasets.find(groupKeyOf(manifest));
        if (found == m_datasets.end()) {
            m_lastError = QStringLiteral("未加载该数据集：%1").arg(groupKeyOf(manifest));
            return false;
        }
        const View3D::LayerId parentGroup = found.value()->parentGroup;
        if (!unloadTileset(manifest))
            return false;
        return loadTilesetInto(manifest, parentGroup);
    }

    // 把"源地心坐标 → 场景局部"的矩阵与原点回填给图层，属性面板据此展示。
    // 球面模式直接按 ECEF 摆放（contentOrigin − 渲染原点），内容不乘这份换算
    // 矩阵，面板也不应再展示它——两种模式的参照系描述因此不同。
    void applyWorldFrame(const View3D::LayerId& layer,
                         const View3DTiles::TilesetWorldFrame::Snapshot& snapshot)
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* layers = container
            ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
            : nullptr;
        if (!layers || layer.isNull() || !snapshot.ready)
            return;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        View3D::ILayer::ReferenceFrame frame;
        if (!scene || !scene->globeModeEnabled()) {
            frame.hasMatrix = true;
            // glm 是列主序；ReferenceFrame::matrix 用行主序（第 r 行第 c 列 = m[c][r]）。
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    frame.matrix[static_cast<std::size_t>(row) * 4 +
                                 static_cast<std::size_t>(column)] =
                        snapshot.ecefToLocal[column][row];
                }
            }
        }
        if (auto* reference = container->objectAs<View3D::IGeoReference>(
                View3D::ServiceIds::GeoReference)) {
            std::array<double, 3> sharedEcef {};
            double lon = 0.0;
            double lat = 0.0;
            double height = 0.0;
            // 只有世界系确实与 crs 服务同源时才标经纬度，避免把别处的原点标错；
            // 本地坐标成果（原点 0）没有地理位置，留空。
            if (reference->originEcef(&sharedEcef) &&
                std::abs(sharedEcef[0] - snapshot.originEcef.x) < 1.0 &&
                std::abs(sharedEcef[1] - snapshot.originEcef.y) < 1.0 &&
                std::abs(sharedEcef[2] - snapshot.originEcef.z) < 1.0 &&
                reference->origin(&lon, &lat, &height)) {
                frame.position = QStringLiteral("%1°, %2°, 椭球高 %3 m")
                                     .arg(lon, 0, 'f', 8)
                                     .arg(lat, 0, 'f', 8)
                                     .arg(height, 0, 'f', 3);
            }
        }
        layers->setLayerReferenceFrame(layer, frame);
    }

    // 世界系对象：创建时就挂上参照系服务，保证原点与影像/二维矢量同源。
    View3DTiles::TilesetWorldFramePtr makeWorldFrame()
    {
        auto frame = std::make_shared<View3DTiles::TilesetWorldFrame>();
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        if (container) {
            frame->setSharedReference(container->objectAs<View3D::IGeoReference>(
                View3D::ServiceIds::GeoReference));
        }
        return frame;
    }

    IApplication* m_application = nullptr;
    View3D::ISceneTreeMenuRegistry* m_menuRegistry = nullptr;
    View3D::IDataOpenRegistry* m_dataOpenRegistry = nullptr;
    View3DTiles::TilesetWorldFramePtr m_frame;
    QHash<QString, std::shared_ptr<Dataset>> m_datasets;
    std::uint64_t m_geoFrameToken = 0;
    std::uint64_t m_globeToken = 0;
    QString m_lastError;
};

void registerTilesetFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.tileset"), QStringLiteral("extension.tileset_loader"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new TilesetExtension(); });
    for (const QString& type : { QStringLiteral("command"), QStringLiteral("checkcommand") }) {
        registry->registerCreator(
            QStringLiteral("plugin.view3d.tileset"), QStringLiteral("factory.create_command"),
            type, [](const PluginCreationContext& context) -> IPlugin* {
                return new TilesetCommand(context);
            });
    }
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.tileset", registerTilesetFactories(registry);)
