#include "View3DPluginEntry.h"

#include "GeoImageryService.h"
#include "GeoNavigationCommand.h"

#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IGeoReference.h>
#include <IPluginContainer.h>
#include <IRenderScene.h>
#include <ISceneLayerService.h>
#include <ISceneTreeMenu.h>
#include <IViewportLayout.h>
#include <View3DObjectIds.h>

#include <QImage>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QStringList>
#include <QUuid>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace RF::PluginEngine;

// 地理能力的宿主接线：发布的都在本扩展生命周期内，影像底图的入口挂在场景树右键
// 菜单上（与「打开数据」同级），不再占用 Ribbon。
// 要素属性表与拾取监听已交给 plugin.view3d.features：属性是跨数据源的，地理插件不该持有。
class GeoExtension final : public IExtension, public View3D::ISceneTreeMenuContributor {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        m_application = application;
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        auto* scene =
            container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene);
        if (!scene)
            return false;
        m_scene = scene;

        // 参照系由 plugin.view3d.crs 拥有并发布（crs 插件不需要任何服务，必先创建成功）；
        // 这里只是消费方，缺失时影像命令会给出明确错误。
        auto* reference =
            container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference);
        m_imagery = std::make_unique<View3DGeo::GeoImageryService>(*scene, reference);

        container->publishAs<View3D::IImageryService>(View3D::ServiceIds::Imagery,
                                                     m_imagery.get(), this);

        // 注册表由宿主发布，先于所有插件创建；这里只消费，不拥有。
        m_menuRegistry = container->objectAs<View3D::ISceneTreeMenuRegistry>(
            View3D::ServiceIds::SceneTreeMenu);
        if (m_menuRegistry)
            m_menuRegistry->addSceneTreeMenuContributor(this);
        ensureGlobeBasemap();
        return true;
    }

    void onDestroy() override
    {
        if (m_scene && m_globeToken != 0)
            m_scene->removeGlobeModeChangeListener(m_globeToken);
        m_globeToken = 0;
        m_basemapNam.reset();
        if (m_menuRegistry)
            m_menuRegistry->removeSceneTreeMenuContributor(this);
        m_menuRegistry = nullptr;
        if (m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                container->unpublishObject(View3D::ServiceIds::Imagery,
                                          static_cast<View3D::IImageryService*>(m_imagery.get()));
            }
        }
        m_imagery.reset();
        m_scene = nullptr;
        m_application = nullptr;
    }

    // 场景树右键菜单：影像底图的入口。地形/DEM 不在这里，走「打开数据」的栅格读取器。
    void appendSceneTreeMenu(const View3D::SceneTreeMenuTarget& target,
                             QList<View3D::SceneTreeMenuAction>* actions) override
    {
        if (!actions)
            return;
        View3D::LayerId parentGroup;
        QString text;
        switch (target.kind) {
        case View3D::SceneTreeMenuTargetKind::SceneRoot:
            text = QStringLiteral("添加在线底图…");
            break;
        case View3D::SceneTreeMenuTargetKind::Group:
            parentGroup = target.primary;
            text = QStringLiteral("添加在线底图到本组…");
            break;
        case View3D::SceneTreeMenuTargetKind::Layer:
        default:
            return;
        }
        View3D::SceneTreeMenuAction addImagery;
        addImagery.text = text;
        addImagery.icon = QStringLiteral("action-imagery-add");
        addImagery.separatorBefore = true;
        addImagery.triggered = [this, parentGroup] { promptAddImageryLayer(parentGroup); };
        actions->append(addImagery);
    }

    // 影像图层登记在 SceneLayerService 里，删除走图层树的通用路径；
    // 服务侧按场景实际存在的 key 惰性清理，因此不声明所有权。
    bool ownsSceneTreeLayer(const View3D::LayerId&) const override { return false; }
    bool removeOwnedSceneTreeLayer(const View3D::LayerId&) override { return false; }

  private:
    // 弹框输入 URL 模板与中心点，调用影像服务提交网格并登记图层。
    void promptAddImageryLayer(const View3D::LayerId& parentGroup)
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* layers = container
            ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
            : nullptr;
        auto* svc = container
            ? container->objectAs<View3D::IImageryService>(View3D::ServiceIds::Imagery)
            : nullptr;
        auto* ref = container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
        if (!svc || !layers)
            return;
        bool ok = false;
        const QString url = QInputDialog::getText(
            nullptr, QStringLiteral("添加在线底图"),
            QStringLiteral("WMTS/TMS/XYZ URL 模板（含 {z}{x}{y}）"), QLineEdit::Normal,
            QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"), &ok);
        if (!ok || url.isEmpty())
            return;
        bool okCenter = false;
        const QString center = QInputDialog::getText(
            nullptr, QStringLiteral("影像中心"),
            QStringLiteral("中心经度,中心纬度（影像将定位到此点，如 116.39,39.91）"),
            QLineEdit::Normal, QStringLiteral("116.39,39.91"), &okCenter);
        double centerLon = 116.39, centerLat = 39.91;
        if (okCenter) {
            const QStringList parts = center.split(QStringLiteral(","), Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                centerLon = parts[0].toDouble();
                centerLat = parts[1].toDouble();
            }
        }
        // 世界原点由第一个地理数据源钉住；已钉住时不会搬动，避免把先加载的数据挪走。
        if (ref)
            ref->setOrigin(centerLon, centerLat, 0.0);
        View3D::ImagerySource src;
        src.name = QStringLiteral("在线底图");
        src.urlTemplate = url;
        src.crs = QStringLiteral("EPSG:3857");
        src.maximumLevel = 18;
        const QString key = QUuid::createUuid().toString();
        const View3D::LayerId layer = layers->addImageryLayer(src.name, key, parentGroup);
        if (!svc->addImageryLayer(src, key) && !layer.isNull())
            layers->removeLayer(layer);
    }

    // ── 球面默认底图（在线影像）─────────────────────────────────────────────
    // 进入球面时异步抓取低层级 XYZ 瓦片（OSM，z=2 共 16 块），拼成墨卡托拼图后
    // 重投影为等距圆柱，替换渲染端的内置静态地球纹理。任何一块失败都保留静态
    // 底图，不影响球面模式本身。VIEW3D_GLOBE_ONLINE_BASEMAP=0/off 可禁用。
    void ensureGlobeBasemap()
    {
        if (!m_scene || m_globeToken != 0)
            return;
        m_globeToken = m_scene->addGlobeModeChangeListener([this](bool enabled) {
            if (enabled)
                fetchOnlineGlobeBasemap();
        });
        if (m_scene->globeModeEnabled())
            fetchOnlineGlobeBasemap();
    }

    void fetchOnlineGlobeBasemap()
    {
        if (m_basemapRequested)
            return;
        const QString mode =
            qEnvironmentVariable("VIEW3D_GLOBE_ONLINE_BASEMAP").trimmed().toLower();
        if (mode == QStringLiteral("0") || mode == QStringLiteral("off")) {
            std::cerr << "VIEW3D_GLOBE_ONLINE_BASEMAP_SKIPPED reason=disabled"
                      << std::endl;
            return;
        }
        m_basemapRequested = true;
        constexpr int level = 2;
        constexpr int gridSize = 1 << level;
        m_basemapTiles = QVector<QImage>(gridSize * gridSize);
        m_basemapPendingTiles = gridSize * gridSize;
        m_basemapNam = std::make_unique<QNetworkAccessManager>();
        for (int ty = 0; ty < gridSize; ++ty) {
            for (int tx = 0; tx < gridSize; ++tx) {
                const int index = ty * gridSize + tx;
                QNetworkRequest request(QUrl(
                    QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
                        .arg(level)
                        .arg(tx)
                        .arg(ty)));
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QStringLiteral("View3D/1.0 (globe basemap)"));
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                     QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply* reply = m_basemapNam->get(request);
                QObject::connect(reply, &QNetworkReply::finished, reply,
                                 [this, reply, index] {
                                     if (!m_basemapNam)
                                         return;
                                     const QByteArray bytes = reply->readAll();
                                     const auto error = reply->error();
                                     reply->deleteLater();
                                     if (error == QNetworkReply::NoError) {
                                         QImage image;
                                         if (image.loadFromData(bytes))
                                             m_basemapTiles[index] = image;
                                     }
                                     if (--m_basemapPendingTiles == 0)
                                         finishOnlineGlobeBasemap();
                                 });
            }
        }
    }

    void finishOnlineGlobeBasemap()
    {
        if (!m_scene)
            return;
        constexpr int gridSize = 4;
        constexpr int tileSize = 256;
        constexpr int mosaicSize = gridSize * tileSize;
        QImage mosaic(mosaicSize, mosaicSize, QImage::Format_RGB888);
        mosaic.fill(QColor(70, 80, 90));
        {
            QPainter painter(&mosaic);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            for (int ty = 0; ty < gridSize; ++ty) {
                for (int tx = 0; tx < gridSize; ++tx) {
                    const QImage tile = m_basemapTiles[ty * gridSize + tx];
                    if (tile.isNull()) {
                        std::cerr << "VIEW3D_GLOBE_ONLINE_BASEMAP_FAILED reason=tile_"
                                  << tx << "_" << ty << std::endl;
                        return;
                    }
                    painter.drawImage(tx * tileSize, ty * tileSize,
                                      tile.scaled(tileSize, tileSize,
                                                  Qt::IgnoreAspectRatio,
                                                  Qt::SmoothTransformation));
                }
            }
        }
        // 墨卡托 → 等距圆柱：输出 2048×1024，行 0 是北极，列 0 是西经 180°。
        constexpr int outWidth = 2048;
        constexpr int outHeight = 1024;
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kMaxMercatorLat = 85.05112878;
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(outWidth) * outHeight * 4u);
        const uchar* mosaicBits = mosaic.constBits();
        const int mosaicStride = static_cast<int>(mosaic.bytesPerLine());
        for (int y = 0; y < outHeight; ++y) {
            double latDeg = 90.0 - (y + 0.5) * 180.0 / outHeight;
            latDeg = std::clamp(latDeg, -kMaxMercatorLat, kMaxMercatorLat);
            const double latRad = latDeg * kPi / 180.0;
            const double mercator = std::log(std::tan(kPi * 0.25 + latRad * 0.5));
            const double v = (kPi - mercator) / (2.0 * kPi);
            const double sourceY = std::clamp(v * mosaicSize - 0.5, 0.0,
                                              static_cast<double>(mosaicSize) - 1.001);
            const int y0 = static_cast<int>(sourceY);
            const double fy = sourceY - y0;
            const uchar* row0 = mosaicBits + y0 * mosaicStride;
            const uchar* row1 = row0 + mosaicStride;
            unsigned char* out = rgba.data() + static_cast<std::size_t>(y) * outWidth * 4u;
            for (int x = 0; x < outWidth; ++x) {
                const double u = (x + 0.5) / outWidth;
                const double sourceX = std::clamp(u * mosaicSize - 0.5, 0.0,
                                                  static_cast<double>(mosaicSize) - 1.001);
                const int x0 = static_cast<int>(sourceX);
                const double fx = sourceX - x0;
                for (int channel = 0; channel < 3; ++channel) {
                    const double top = row0[x0 * 3 + channel] * (1.0 - fx) +
                        row0[(x0 + 1) * 3 + channel] * fx;
                    const double bottom = row1[x0 * 3 + channel] * (1.0 - fx) +
                        row1[(x0 + 1) * 3 + channel] * fx;
                    out[x * 4 + channel] = static_cast<unsigned char>(
                        std::clamp(top * (1.0 - fy) + bottom * fy, 0.0, 255.0));
                }
                out[x * 4 + 3] = 255;
            }
        }
        if (!m_scene->setGlobeBasemapImage(outWidth, outHeight, rgba)) {
            std::cerr << "VIEW3D_GLOBE_ONLINE_BASEMAP_FAILED reason=submit" << std::endl;
            return;
        }
        std::cerr << "VIEW3D_GLOBE_ONLINE_BASEMAP_OK tiles=" << (gridSize * gridSize)
                  << " size=" << outWidth << "x" << outHeight << std::endl;
    }

    IApplication* m_application = nullptr;
    View3D::IRenderScene* m_scene = nullptr;
    View3D::ISceneTreeMenuRegistry* m_menuRegistry = nullptr;
    std::unique_ptr<View3DGeo::GeoImageryService> m_imagery;
    std::uint64_t m_globeToken = 0;
    bool m_basemapRequested = false;
    int m_basemapPendingTiles = 0;
    QVector<QImage> m_basemapTiles;
    std::unique_ptr<QNetworkAccessManager> m_basemapNam;
};

IPlugin* createGeoCommand(const PluginCreationContext& context)
{
    const QString name = context.attributes.value(QStringLiteral("name"));
    const QString caption = context.attributes.value(QStringLiteral("caption"));
    if (name == QStringLiteral("cmd.navigate.coordinate"))
        return new View3DGeo::GeoNavigationCommand(name, caption,
                                                  View3DGeo::NavTool::Coordinate);
    if (name == QStringLiteral("cmd.navigate.geosearch"))
        return new View3DGeo::GeoNavigationCommand(name, caption,
                                                  View3DGeo::NavTool::GeoSearch);
    if (name == QStringLiteral("cmd.navigate.flyto"))
        return new View3DGeo::GeoNavigationCommand(name, caption,
                                                  View3DGeo::NavTool::FlyTo);
    return nullptr;
}

void registerGeoFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.geo"), QStringLiteral("extension.geo"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new GeoExtension(); });
    for (const QString& type : {QStringLiteral("command"), QStringLiteral("checkcommand")}) {
        registry->registerCreator(QStringLiteral("plugin.view3d.geo"),
                                  QStringLiteral("factory.create_command"), type,
                                  createGeoCommand);
    }
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.geo", registerGeoFactories(registry);)
