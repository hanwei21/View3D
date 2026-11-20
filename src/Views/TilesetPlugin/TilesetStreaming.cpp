#include "TilesetStreaming.h"

#include "TilesetAccessor.h"
#include "TilesetGeometry.h"

#include <IGeoReference.h>
#include <GlobeMath.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <ISceneContent.h>
#include <IViewportCamera.h>
#include <IViewportLayout.h>

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileID.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetLoadFailureDetails.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumGeometry/OrientedBoundingBox.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/GlobeTransforms.h>
#include <CesiumGltf/Buffer.h>
#include <CesiumGltf/Image.h>
#include <CesiumGltf/Model.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumImage/ImageAsset.h>
#include <CesiumRasterOverlays/RasterOverlayTile.h>
#include <CesiumUtility/Math.h>

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <any>
#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace View3DTiles {
namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

class TilesetTaskProcessor final : public CesiumAsync::ITaskProcessor {
  public:
    void startTask(std::function<void()> task) override
    {
        QThreadPool::globalInstance()->start(std::move(task));
    }
};

struct TileResource {
    std::shared_ptr<const View3D::ModelMeshDto> mesh;
    std::shared_ptr<std::atomic_bool> released =
        std::make_shared<std::atomic_bool>(false);
    QString key;
    std::size_t bytes = 0;
    std::int64_t modelBytes = 0;
};

// 与 Cesium Tile::computeByteSize 同口径：glTF buffer + 解码后的图像资产。
// 我们用它把"模型缓存上限"换算成 DTO 预算，因为 DTO（双精度顶点 + RGBA8
// 贴图 + 射线索引）比 Cesium 自己记账的内容大一个数量级。
std::int64_t modelDataBytes(const CesiumGltf::Model& model)
{
    std::int64_t bytes = 0;
    for (const CesiumGltf::Buffer& buffer : model.buffers)
        bytes += static_cast<std::int64_t>(buffer.cesium.data.size());
    for (const CesiumGltf::Image& image : model.images) {
        if (image.bufferView >= 0 &&
            static_cast<std::size_t>(image.bufferView) < model.bufferViews.size())
            bytes -= static_cast<std::int64_t>(
                model.bufferViews[static_cast<std::size_t>(image.bufferView)].byteLength);
        if (image.pAsset) {
            const std::int64_t size = image.pAsset->sizeBytes >= 0
                ? image.pAsset->sizeBytes
                : static_cast<std::int64_t>(image.pAsset->pixelData.size());
            bytes += size;
        }
    }
    return std::max<std::int64_t>(0, bytes);
}

QString tileIdString(const Cesium3DTilesSelection::Tile& tile)
{
    const std::string id =
        Cesium3DTilesSelection::TileIdUtilities::createTileIdString(tile.getTileID());
    return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

glm::dvec3 transformPoint(const glm::dmat4& matrix, const std::array<double, 3>& point)
{
    const glm::dvec4 value = matrix * glm::dvec4(point[0], point[1], point[2], 1.0);
    return glm::dvec3(value.x, value.y, value.z) / (std::abs(value.w) > 1e-12 ? value.w : 1.0);
}

glm::dvec3 normalizeOr(const glm::dvec3& value, const glm::dvec3& fallback)
{
    const double length = glm::length(value);
    return length > 1e-12 ? value / length : fallback;
}

} // namespace

void TilesetWorldFrame::setSharedReference(View3D::IGeoReference* reference)
{
    std::scoped_lock lock(m_mutex);
    m_sharedReference = reference;
}

bool TilesetWorldFrame::ensureInitialized(const glm::dvec3& originEcef)
{
    std::scoped_lock lock(m_mutex);
    if (m_ready)
        return true;
    if (!std::isfinite(originEcef.x) || !std::isfinite(originEcef.y) ||
        !std::isfinite(originEcef.z))
        return false;
    const double length = glm::length(originEcef);
    if (length < 1.0e5) {
        // 不是有效的地心坐标（本地坐标格式的成果）：直接按恒等变换显示。
        m_ready = true;
        m_originEcef = glm::dvec3(0.0);
        m_ecefToLocal = glm::dmat4(1.0);
        m_localToEcef = glm::dmat4(1.0);
        return true;
    }
    // 与 crs 服务对表世界原点：已钉住就直接领用（多源叠加才不错位）；
    // 还没有则用本数据集的第一个中心点把它钉住，后到的数据源不会再挪。
    glm::dvec3 frameOrigin = originEcef;
    if (m_sharedReference) {
        std::array<double, 3> shared {};
        if (m_sharedReference->originEcef(&shared)) {
            frameOrigin = glm::dvec3(shared[0], shared[1], shared[2]);
        } else if (m_sharedReference->setOriginFromEcef(
                       { originEcef.x, originEcef.y, originEcef.z }) &&
                   m_sharedReference->originEcef(&shared)) {
            frameOrigin = glm::dvec3(shared[0], shared[1], shared[2]);
        }
    }
    const glm::dmat4 enuToEcef =
        CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(
            frameOrigin, CesiumGeospatial::Ellipsoid::WGS84);
    // ENU(east,north,up) -> View3D(east,up,-north)：行列式 +1 的纯旋转。
    const glm::dmat4 view3dFromEnu(
        glm::dvec4(1.0, 0.0, 0.0, 0.0), glm::dvec4(0.0, 0.0, -1.0, 0.0),
        glm::dvec4(0.0, 1.0, 0.0, 0.0), glm::dvec4(0.0, 0.0, 0.0, 1.0));
    m_ecefToLocal = view3dFromEnu * glm::inverse(enuToEcef);
    m_localToEcef = glm::inverse(m_ecefToLocal);
    m_originEcef = frameOrigin;
    m_ready = true;
    return true;
}

bool TilesetWorldFrame::ready() const
{
    std::scoped_lock lock(m_mutex);
    return m_ready;
}

TilesetWorldFrame::Snapshot TilesetWorldFrame::snapshot() const
{
    std::scoped_lock lock(m_mutex);
    Snapshot result;
    result.ready = m_ready;
    result.originEcef = m_originEcef;
    result.ecefToLocal = m_ecefToLocal;
    result.localToEcef = m_localToEcef;
    return result;
}

namespace {

class TilesetRendererResources final
    : public Cesium3DTilesSelection::IPrepareRendererResources {
  public:
    struct Snapshot {
        std::uint64_t prepared = 0;
        std::uint64_t mainPrepared = 0;
        std::uint64_t freed = 0;
        std::uint64_t live = 0;
        std::uint64_t emptyModels = 0;
        std::uint64_t dtoBytes = 0;
        std::uint64_t modelBytes = 0;
        TilesetConvertStats conversion;
    };

    TilesetRendererResources(TilesetWorldFramePtr frame, TilesetConvertOptions options,
                             bool globeMode)
        : m_frame(std::move(frame)), m_options(options), m_globeMode(globeMode)
    {
    }

    CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
    prepareInLoadThread(const CesiumAsync::AsyncSystem& asyncSystem,
                        Cesium3DTilesSelection::TileLoadResult&& result,
                        const glm::dmat4& transform, const std::any& rendererOptions) override
    {
        static_cast<void>(rendererOptions);
        std::shared_ptr<const View3D::ModelMeshDto> mesh;
        TilesetConvertStats conversion;
        std::int64_t modelBytes = 0;
        // Cesium 的加载线程没有为我们兜底：转换、矩阵、glTF 扩展或分配
        // 任何一处抛异常都会直接穿到线程顶层，表现为 0xE06D7363/堆损坏。
        // 这里把整段内容准备都包住，失败就退化成空瓦片，绝不跨线程抛出。
        try {
        if (auto* model = std::get_if<CesiumGltf::Model>(&result.contentKind)) {
            modelBytes = modelDataBytes(*model);
            glm::dvec3 origin = glm::dvec3(transform[3]);
            if (result.initialBoundingVolume) {
                origin = Cesium3DTilesSelection::getBoundingVolumeCenter(
                    *result.initialBoundingVolume);
            }
            // 首块内容可能在主线程看到根 Tile 之前就开始转换；这里用瓦片自身的
            // 包围体中心兜底初始化世界系，保证转换用的局部原点与相机一致。
            m_frame->ensureInitialized(origin);
            const TilesetWorldFrame::Snapshot frame = m_frame->snapshot();
            if (frame.ready) {
                glm::dmat4 contentTransform =
                    CesiumGltfContent::GltfUtilities::applyRtcCenter(*model, transform);
                contentTransform = CesiumGltfContent::GltfUtilities::applyGltfUpAxisTransform(
                    *model, contentTransform);
                glm::dvec3 contentOrigin { 0.0 };
                if (m_globeMode) {
                    // 球面烘焙：平移列清零后只剩旋转/缩放，float 分解安全；
                    // ECEF 原点记入 DTO，渲染端按 contentOrigin − renderOrigin 摆放。
                    contentOrigin = glm::dvec3(contentTransform[3]);
                    contentTransform[3] = glm::dvec4(0.0, 0.0, 0.0, 1.0);
                } else {
                    contentTransform = frame.ecefToLocal * contentTransform;
                }
                try {
                    std::shared_ptr<View3D::ModelMeshDto> converted =
                        convertModelToMesh(*model, contentTransform, m_options,
                                           &conversion);
                    if (converted && m_globeMode) {
                        converted->hasContentOrigin = true;
                        converted->contentOrigin = { contentOrigin.x, contentOrigin.y,
                                                     contentOrigin.z };
                    }
                    mesh = std::move(converted);
                } catch (const std::exception& error) {
                    conversion.warnings.append(
                        QStringLiteral("瓦片模型转换异常：%1")
                            .arg(QString::fromUtf8(error.what())));
                } catch (...) {
                    conversion.warnings.append(QStringLiteral("瓦片模型转换异常"));
                }
            } else {
                ++m_noFrame;
            }
        }
        } catch (const std::exception& error) {
            mesh.reset();
            try {
                conversion.warnings.append(
                    QStringLiteral("瓦片内容准备异常：%1")
                        .arg(QString::fromUtf8(error.what())));
            } catch (...) {
            }
        } catch (...) {
            mesh.reset();
        }

        TileResource* resource = nullptr;
        try {
            resource = new (std::nothrow) TileResource;
            if (resource) {
                resource->mesh = std::move(mesh);
                resource->bytes = resource->mesh
                    ? View3D::modelMeshCpuBytes(*resource->mesh) : 0;
                resource->modelBytes = modelBytes;
                std::scoped_lock lock(m_resourceMutex);
                m_resources.insert(resource);
            }
        } catch (...) {
            delete resource;
            resource = nullptr;
        }

        ++m_prepared;
        if (!resource) {
            ++m_emptyModels;
        } else {
            try {
                std::scoped_lock lock(m_mutex);
                m_stats += conversion;
                if (m_stats.warnings.size() > 200)
                    m_stats.warnings = m_stats.warnings.mid(0, 200);
            } catch (...) {
            }
            if (!resource->mesh)
                ++m_emptyModels;
            else
                m_dtoBytes += resource->bytes;
            m_modelBytes += static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, modelBytes));
            ++m_live;
        }
        return asyncSystem.createResolvedFuture(
            Cesium3DTilesSelection::TileLoadResultAndRenderResources {
                std::move(result), resource });
    }

    void* prepareInMainThread(Cesium3DTilesSelection::Tile& tile, void* load) override
    {
        if (!load)
            return nullptr;
        {
            std::scoped_lock lock(m_resourceMutex);
            if (m_resources.find(static_cast<TileResource*>(load)) ==
                m_resources.end())
                return nullptr;
        }
        try {
            static_cast<TileResource*>(load)->key = tileIdString(tile);
        } catch (...) {
            // key 只是诊断用；失败时 syncScene 会按 Tile ID 重新计算。
        }
        ++m_mainPrepared;
        return load;
    }

    void free(Cesium3DTilesSelection::Tile& tile, void* load, void* main) noexcept override
    {
        static_cast<void>(tile);
        void* target = main ? main : load;
        if (!target)
            return;
        // Cesium 在内容替换/瓦片销毁的边界上可能重复回调 free；用登记表
        // 做幂等，避免同一 TileResource 被 delete 两次造成堆损坏。
        bool owned = false;
        try {
            std::scoped_lock lock(m_resourceMutex);
            const auto found =
                m_resources.find(static_cast<TileResource*>(target));
            if (found != m_resources.end()) {
                m_resources.erase(found);
                owned = true;
            }
        } catch (...) {
            return;
        }
        if (!owned)
            return;
        auto* resource = static_cast<TileResource*>(target);
        if (resource->released)
            resource->released->store(true);
        const std::size_t bytes = resource->bytes;
        if (m_dtoBytes >= bytes)
            m_dtoBytes -= bytes;
        const std::uint64_t modelBytes =
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, resource->modelBytes));
        if (m_modelBytes >= modelBytes)
            m_modelBytes -= modelBytes;
        delete resource;
        ++m_freed;
        if (m_live > 0)
            --m_live;
    }

    void* prepareRasterInLoadThread(CesiumImage::ImageAsset&, const std::any&) override
    {
        return nullptr;
    }
    void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile&, void*) override
    {
        return nullptr;
    }
    void freeRaster(const CesiumRasterOverlays::RasterOverlayTile&, void*, void*) noexcept override
    {
    }
    void attachRasterInMainThread(const Cesium3DTilesSelection::Tile&, std::int32_t,
                                  const CesiumRasterOverlays::RasterOverlayTile&, void*,
                                  const glm::dvec2&, const glm::dvec2&) override
    {
    }
    void detachRasterInMainThread(const Cesium3DTilesSelection::Tile&, std::int32_t,
                                  const CesiumRasterOverlays::RasterOverlayTile&,
                                  void*) noexcept override
    {
    }

    std::uint64_t dtoBytes() const noexcept { return m_dtoBytes.load(); }
    std::uint64_t modelBytes() const noexcept { return m_modelBytes.load(); }

    Snapshot snapshot() const
    {
        Snapshot result;
        result.prepared = m_prepared.load();
        result.mainPrepared = m_mainPrepared.load();
        result.freed = m_freed.load();
        result.live = m_live.load();
        result.emptyModels = m_emptyModels.load();
        result.dtoBytes = m_dtoBytes.load();
        result.modelBytes = m_modelBytes.load();
        {
            std::scoped_lock lock(m_mutex);
            result.conversion = m_stats;
        }
        return result;
    }

  private:
    TilesetWorldFramePtr m_frame;
    TilesetConvertOptions m_options;
    // 会话生命周期内不变：模式切换走 stop→set→start，不在会话中途翻转换路径。
    const bool m_globeMode;
    mutable std::mutex m_mutex;
    TilesetConvertStats m_stats;
    std::atomic<std::uint64_t> m_prepared { 0 };
    std::atomic<std::uint64_t> m_mainPrepared { 0 };
    std::atomic<std::uint64_t> m_freed { 0 };
    std::atomic<std::uint64_t> m_live { 0 };
    std::atomic<std::uint64_t> m_emptyModels { 0 };
    std::atomic<std::uint64_t> m_dtoBytes { 0 };
    std::atomic<std::uint64_t> m_modelBytes { 0 };
    std::atomic<std::uint64_t> m_noFrame { 0 };
    std::mutex m_resourceMutex;
    std::unordered_set<const TileResource*> m_resources;
};

} // namespace

struct TilesetStreaming::Impl {
    struct SceneTile {
        std::shared_ptr<const View3D::ModelMeshDto> mesh;
        std::shared_ptr<std::atomic_bool> released;
        std::int64_t modelBytes = 0;
        bool visible = false;
        qint64 lastSelectedMs = 0;
    };

    struct SelectedTile {
        std::shared_ptr<const View3D::ModelMeshDto> mesh;
        std::shared_ptr<std::atomic_bool> released;
        std::int64_t modelBytes = 0;
    };

    Impl(View3D::IRenderScene& scene, View3D::IViewportLayout* layout,
         TilesetWorldFramePtr frame, QString tilesetPath, QString groupKey, QString displayName,
         View3D::TileCacheSettings settings)
        : m_scene(scene), m_layout(layout), m_frame(std::move(frame)),
          m_path(std::move(tilesetPath)), m_groupKey(std::move(groupKey)),
          m_displayName(std::move(displayName)), m_settings(settings),
          m_tasks(std::make_shared<TilesetTaskProcessor>()),
          m_asyncSystem(m_tasks)
    {
        m_settings.clampToValidRange();
        m_convertOptions.unlit = !qEnvironmentVariableIsSet("VIEW3D_TILES_LIT");
        m_convertOptions.buildRayIndex = !qEnvironmentVariableIsSet("VIEW3D_TILES_NO_RAY_INDEX");
        m_tick.setInterval(33);
        QObject::connect(&m_tick, &QTimer::timeout, &m_context, [this] { tick(); });
    }

    bool start()
    {
        if (m_path.isEmpty())
            return false;
        const QFileInfo manifest(m_path);
        if (!manifest.exists() || !manifest.isFile()) {
            m_error = QStringLiteral("找不到 tileset.json：%1").arg(m_path);
            return false;
        }
        m_accessor = std::make_shared<TilesetAssetAccessor>(manifest.absolutePath());
        m_renderer =
            std::make_shared<TilesetRendererResources>(m_frame, m_convertOptions, m_globeMode);

        static std::once_flag registration;
        std::call_once(registration, [] {
            Cesium3DTilesContent::registerAllTileContentTypes();
        });

        Cesium3DTilesSelection::TilesetOptions options;
        options.maximumScreenSpaceError = m_settings.screenSpaceErrorPixels;
        // Cesium 的缓存口径只包含内容本身；DTO 与显存各有一份，取一半作为总预算。
        options.maximumCachedBytes = static_cast<std::int64_t>(
            std::max<std::uint64_t>(64ull << 20, m_settings.cacheBytes / 2ull));
        options.maximumSimultaneousTileLoads =
            static_cast<std::uint32_t>(std::max(2, m_settings.workers));
        options.loadingDescendantLimit =
            static_cast<std::uint32_t>(std::max(8, m_settings.maxInFlight));
        // CesiumJS 的 3D Tiles 默认不预取兄弟；0.62 的原生默认是 true。
        options.preloadSiblings = false;
        options.preloadAncestors = true;
        options.forbidHoles = false;
        options.enableFrustumCulling = true;
        options.enableFogCulling = false;
        options.delayRefinementForOcclusion = false;
        options.mainThreadLoadingTimeLimit = 8.0;
        options.tileCacheUnloadTimeLimit = 2.0;
        options.contentOptions.generateMissingNormalsSmooth = false;
        options.ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
        m_baseSse = options.maximumScreenSpaceError;
        m_maxSse = 512.0;
        m_sseEffective = m_baseSse;
        m_initialModelCache = options.maximumCachedBytes;
        const QString logPath = m_path;
        options.loadErrorCallback = [logPath](
                                         const Cesium3DTilesSelection::TilesetLoadFailureDetails&
                                             details) {
            std::cerr << "VIEW3D_TILES_LOAD_ERROR path=" << logPath.toStdString()
                      << " message=" << details.message << " status=" << details.statusCode
                      << std::endl;
        };

        try {
            Cesium3DTilesSelection::TilesetExternals externals {
                m_accessor, m_renderer, m_asyncSystem, nullptr };
            m_tileset = std::make_unique<Cesium3DTilesSelection::Tileset>(
                externals, manifest.fileName().toStdString(), options);
        } catch (const std::exception& error) {
            m_error = QStringLiteral("创建 Tileset 失败：%1")
                          .arg(QString::fromUtf8(error.what()));
            std::cerr << "VIEW3D_TILES_START_FAILED reason="
                      << m_error.toStdString() << std::endl;
            return false;
        }
        m_startMs = nowMs();
        m_lastUpdateMs = m_startMs;
        m_lastLogMs = m_startMs;
        m_tick.start();
        std::cerr << "VIEW3D_TILES_START group=" << m_groupKey.toStdString()
                  << " path=" << m_path.toStdString()
                  << " sse=" << m_settings.screenSpaceErrorPixels
                  << " cache_mb=" << (m_settings.cacheBytes / (1024.0 * 1024.0))
                  << " preset=" << m_settings.preset.toStdString() << std::endl;
        return true;
    }

    void stop()
    {
        m_tick.stop();
        detachFromScene();
        if (m_tileset) {
            CesiumAsync::SharedFuture<void>& destroyed =
                m_tileset->getAsyncDestructionCompleteEvent();
            m_tileset.reset();
            const qint64 deadline = nowMs() + 10000;
            while (!destroyed.isReady() && nowMs() < deadline) {
                m_asyncSystem.dispatchMainThreadTasks();
                QThread::msleep(1);
            }
            m_asyncSystem.dispatchMainThreadTasks();
            if (!destroyed.isReady())
                std::cerr << "VIEW3D_TILES_STOP_TIMEOUT group=" << m_groupKey.toStdString()
                          << std::endl;
        }
        m_renderer.reset();
        m_accessor.reset();
    }

    void detachFromScene()
    {
        m_scene.clearTileGroup(m_groupKey);
        m_sceneTiles.clear();
        m_selectedKeys.clear();
        m_visibleTiles = 0;
        m_residentTiles = 0;
        m_framed = false;
    }

    void tick()
    {
        if (!m_tileset)
            return;
        const qint64 now = nowMs();
        const double deltaSeconds =
            std::clamp(static_cast<double>(now - m_lastUpdateMs) / 1000.0, 0.0, 0.2);
        m_lastUpdateMs = now;

        try {
            m_asyncSystem.dispatchMainThreadTasks();
            refreshRoot();
            // 相机还没框到数据集之前，默认相机通常站在包围体中心附近；
            // 那几轮会把周围最深层的瓦片全部请求下来。与倾斜摄影一致，
            // 先在根包围体推出的引导视角下加载，focus 完成后再切到真实相机。
            updateMemoryBudget(now);
            std::vector<Cesium3DTilesSelection::ViewState> views;
            if (m_framed)
                views = buildViews();
            if (views.empty())
                views.push_back(bootstrapView());
            const Cesium3DTilesSelection::ViewUpdateResult& update =
                m_tileset->updateViewGroup(m_tileset->getDefaultViewGroup(), views,
                                           static_cast<float>(deltaSeconds));
            m_asyncSystem.dispatchMainThreadTasks();
            m_tileset->loadTiles();
            m_asyncSystem.dispatchMainThreadTasks();
            refreshRoot();
            syncScene(update, now);
            logProgress(now);
        } catch (const std::exception& error) {
            m_error = QStringLiteral("瓦片流更新异常：%1")
                          .arg(QString::fromUtf8(error.what()));
            m_tick.stop();
            std::cerr << "VIEW3D_TILES_STREAM_FAILED group=" << m_groupKey.toStdString()
                      << " reason=" << m_error.toStdString() << std::endl;
        } catch (...) {
            m_error = QStringLiteral("瓦片流更新异常");
            m_tick.stop();
            std::cerr << "VIEW3D_TILES_STREAM_FAILED group=" << m_groupKey.toStdString()
                      << " reason=unknown" << std::endl;
        }
    }

    void refreshRoot()
    {
        if (!m_tileset)
            return;
        const Cesium3DTilesSelection::Tile* root = m_tileset->getRootTile();
        if (!root)
            return;
        if (!m_rooted) {
            m_rooted = true;
            std::cerr << "VIEW3D_TILES_ROOT group=" << m_groupKey.toStdString()
                      << " ms=" << (nowMs() - m_startMs) << std::endl;
        }
        if (!m_frame->ready()) {
            const glm::dvec3 center =
                Cesium3DTilesSelection::getBoundingVolumeCenter(root->getBoundingVolume());
            m_frame->ensureInitialized(center);
            const TilesetWorldFrame::Snapshot snapshot = m_frame->snapshot();
            if (snapshot.ready) {
                std::cerr << "VIEW3D_TILES_FRAME group=" << m_groupKey.toStdString()
                          << " ecef=" << snapshot.originEcef.x << ","
                          << snapshot.originEcef.y << "," << snapshot.originEcef.z
                          << std::endl;
            }
        }
        if (m_frameCallback && !m_frameReported) {
            const TilesetWorldFrame::Snapshot snapshot = m_frame->snapshot();
            if (snapshot.ready) {
                m_frameReported = true;
                m_frameCallback(snapshot);
            }
        }
        // 根瓦片一到就用根包围盒复位相机/枢轴：不能等场景瓦片就绪再看
        // focusTileGroup。慢数据或反复加载时，旧数据集的 pivot 会一直保留，
        // 用户旋转/缩放会围绕旧中心，表现为"复位范围不对、操作被卡住"。
        if (!m_framed)
            applyRootView();
    }

    bool applyRootView()
    {
        if (!m_tileset || !m_layout)
            return false;
        const Cesium3DTilesSelection::Tile* root = m_tileset->getRootTile();
        if (!root)
            return false;
        const TilesetWorldFrame::Snapshot frame = m_frame->snapshot();
        if (!frame.ready)
            return false;

        const glm::dvec3 centerEcef =
            Cesium3DTilesSelection::getBoundingVolumeCenter(root->getBoundingVolume());
        double radius = 1.0;
        try {
            const CesiumGeometry::OrientedBoundingBox box =
                Cesium3DTilesSelection::getOrientedBoundingBoxFromBoundingVolume(
                    root->getBoundingVolume(), CesiumGeospatial::Ellipsoid::WGS84);
            const glm::dvec3 lengths = box.getLengths();
            radius = std::max({ lengths.x, lengths.y, lengths.z, 0.2 }) * 0.5;
        } catch (...) {
        }
        glm::dvec3 position;
        glm::dvec3 direction;
        glm::dvec3 up;
        if (m_globeMode) {
            // 球面取景：站在数据集中心的椭球法线方向，状态直接写 ECEF。
            const View3D::GlobeMath::Vec3 center { centerEcef.x, centerEcef.y, centerEcef.z };
            const View3D::GlobeMath::Vec3 normal = View3D::GlobeMath::ecefNormal(center);
            View3D::GlobeMath::Vec3 east = View3D::GlobeMath::normalized(
                View3D::GlobeMath::cross({ 0.0, 0.0, 1.0 }, normal));
            if (View3D::GlobeMath::length(east) < 1e-6)
                east = { 1.0, 0.0, 0.0 };
            const View3D::GlobeMath::Vec3 eye = View3D::GlobeMath::add(
                center, View3D::GlobeMath::add(
                            View3D::GlobeMath::scale(normal, radius * 2.5),
                            View3D::GlobeMath::scale(east, radius * 0.35)));
            position = glm::dvec3(eye[0], eye[1], eye[2]);
            direction = normalizeOr(centerEcef - position,
                                    glm::dvec3(-normal[0], -normal[1], -normal[2]));
            up = glm::dvec3(normal[0], normal[1], normal[2]);
        } else {
            const glm::dvec3 center = transformPoint(
                frame.ecefToLocal,
                std::array<double, 3> { centerEcef.x, centerEcef.y, centerEcef.z });
            position = center + glm::dvec3(0.0, radius * 0.35, radius * 2.5);
            direction = normalizeOr(center - position, glm::dvec3(0.0, 0.0, -1.0));
            up = glm::dvec3(0.0, 1.0, 0.0);
        }

        QList<View3D::IRenderSurface*> views = m_layout->views();
        if (views.isEmpty()) {
            if (View3D::IRenderSurface* active = m_layout->activeView())
                views.append(active);
            else if (View3D::IRenderSurface* primary = m_layout->primaryView())
                views.append(primary);
        }
        bool applied = false;
        for (View3D::IRenderSurface* view : views) {
            if (!view)
                continue;
            View3D::ViewportCameraState state;
            if (!view->cameraState(&state))
                continue;
            state.position = {position.x, position.y, position.z};
            state.direction = {direction.x, direction.y, direction.z};
            state.up = {up.x, up.y, up.z};
            if (m_globeMode) {
                state.target = {centerEcef.x, centerEcef.y, centerEcef.z};
                state.orthographic = false;
            } else {
                const glm::dvec3 center = transformPoint(
                    frame.ecefToLocal,
                    std::array<double, 3> { centerEcef.x, centerEcef.y, centerEcef.z });
                state.target = {center.x, center.y, center.z};
            }
            if (state.viewport[0] <= 1.0 || state.viewport[1] <= 1.0) {
                const QSize pixels = view->renderPixelSize();
                state.viewport = { static_cast<double>(std::max(1, pixels.width())),
                                   static_cast<double>(std::max(1, pixels.height())) };
            }
            if (state.orthographic && state.orthographicHeight <= 0.01)
                state.orthographicHeight = radius * 2.5;
            if (view->setCameraState(state))
                applied = true;
        }
        if (!applied)
            return false;
        m_framed = true;
        std::cerr << "VIEW3D_TILES_FRAMED group=" << m_groupKey.toStdString()
                  << " ms=" << (nowMs() - m_startMs)
                  << " source=root radius=" << radius << std::endl;
        return true;
    }

    std::vector<Cesium3DTilesSelection::ViewState> buildViews() const
    {
        std::vector<Cesium3DTilesSelection::ViewState> result;
        const TilesetWorldFrame::Snapshot frame = m_frame->snapshot();
        if (!frame.ready || !m_layout)
            return result;
        QList<View3D::IRenderSurface*> views = m_layout->views();
        if (views.isEmpty()) {
            if (View3D::IRenderSurface* active = m_layout->activeView())
                views.append(active);
            else if (View3D::IRenderSurface* primary = m_layout->primaryView())
                views.append(primary);
        }
        for (View3D::IRenderSurface* view : views) {
            if (!view)
                continue;
            View3D::ViewportCameraState state;
            if (!view->cameraState(&state))
                continue;
            glm::dvec3 position;
            glm::dvec3 direction;
            glm::dvec3 up;
            if (m_globeMode) {
                // 球面相机状态本身就是 ECEF，不需要局部→ECEF 换算。
                position = glm::dvec3(state.position[0], state.position[1], state.position[2]);
                direction = glm::dvec3(state.direction[0], state.direction[1],
                                       state.direction[2]);
                up = glm::dvec3(state.up[0], state.up[1], state.up[2]);
            } else {
                position = transformPoint(frame.localToEcef, state.position);
                const glm::dmat3 rotation = glm::dmat3(frame.localToEcef);
                direction = rotation * glm::dvec3(
                    state.direction[0], state.direction[1], state.direction[2]);
                up = rotation * glm::dvec3(state.up[0], state.up[1], state.up[2]);
            }
            direction = normalizeOr(direction, glm::dvec3(0.0, 0.0, -1.0));
            up = normalizeOr(up, glm::dvec3(0.0, 1.0, 0.0));
            const QSize pixelSize = view->renderPixelSize();
            // CesiumJS 的 SSE 用绘制缓冲尺寸（设备像素）；高 DPI 下逻辑像素
            // 会低估误差，表现为总是比应有的层级粗一档。
            const double width = pixelSize.width() > 1
                ? pixelSize.width()
                : (state.viewport[0] > 1.0 ? state.viewport[0] : 1280.0);
            const double height = pixelSize.height() > 1
                ? pixelSize.height()
                : (state.viewport[1] > 1.0 ? state.viewport[1] : 720.0);
            if (state.orthographic && state.orthographicHeight > 0.01) {
                const double halfHeight = state.orthographicHeight * 0.5;
                const double halfWidth = halfHeight * (width / height);
                result.emplace_back(position, direction, up, glm::dvec2(width, height),
                                    -halfWidth, halfWidth, -halfHeight, halfHeight,
                                    CesiumGeospatial::Ellipsoid::WGS84);
            } else {
                const double vertical =
                    state.verticalFovRadians > 0.01 ? state.verticalFovRadians : 0.7853982;
                const double horizontal = state.horizontalFovRadians > 0.01
                    ? state.horizontalFovRadians
                    : 2.0 * std::atan(std::tan(vertical * 0.5) * (width / height));
                result.emplace_back(position, direction, up, glm::dvec2(width, height),
                                    horizontal, vertical,
                                    CesiumGeospatial::Ellipsoid::WGS84);
            }
        }
        return result;
    }

    Cesium3DTilesSelection::ViewState bootstrapView() const
    {
        using namespace CesiumGeospatial;
        using CesiumUtility::Math;
        QSize pixelSize(1280, 720);
        if (m_layout) {
            View3D::IRenderSurface* view = m_layout->activeView();
            if (!view)
                view = m_layout->primaryView();
            if (view) {
                const QSize size = view->renderPixelSize();
                if (size.width() > 1 && size.height() > 1)
                    pixelSize = size;
            }
        }
        const glm::dvec2 viewport(pixelSize.width(), pixelSize.height());
        if (m_tileset) {
            const Cesium3DTilesSelection::Tile* root = m_tileset->getRootTile();
            if (root) {
                try {
                    const Cesium3DTilesSelection::BoundingVolume& volume =
                        root->getBoundingVolume();
                    const glm::dvec3 center =
                        Cesium3DTilesSelection::getBoundingVolumeCenter(volume);
                    const CesiumGeometry::OrientedBoundingBox box =
                        Cesium3DTilesSelection::getOrientedBoundingBoxFromBoundingVolume(
                            volume, Ellipsoid::WGS84);
                    const glm::dvec3 lengths = box.getLengths();
                    const double radius =
                        std::max({ lengths.x, lengths.y, lengths.z, 1.0 }) * 0.5;
                    glm::dvec3 up = normalizeOr(center, glm::dvec3(0.0, 0.0, 1.0));
                    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
                    if (glm::length(east) < 1e-6)
                        east = glm::cross(glm::dvec3(1.0, 0.0, 0.0), up);
                    east = normalizeOr(east, glm::dvec3(1.0, 0.0, 0.0));
                    const glm::dvec3 eye =
                        center + up * (radius * 2.5) - east * (radius * 0.6);
                    const glm::dvec3 direction = normalizeOr(center - eye, -up);
                    glm::dvec3 viewUp = up - direction * glm::dot(up, direction);
                    viewUp = normalizeOr(viewUp, east);
                    return Cesium3DTilesSelection::ViewState(
                        eye, direction, viewUp, viewport,
                        Math::degreesToRadians(60.0), Math::degreesToRadians(40.0),
                        Ellipsoid::WGS84);
                } catch (...) {
                }
            }
        }
        const Cartographic eyeCartographic {
            Math::degreesToRadians(10.0), Math::degreesToRadians(45.0), 30.0
        };
        const Cartographic targetCartographic {
            Math::degreesToRadians(10.0), Math::degreesToRadians(45.0), 0.0
        };
        const glm::dvec3 eye =
            Ellipsoid::WGS84.cartographicToCartesian(eyeCartographic);
        const glm::dvec3 target =
            Ellipsoid::WGS84.cartographicToCartesian(targetCartographic);
        return Cesium3DTilesSelection::ViewState(
            eye, normalizeOr(target - eye, glm::dvec3(0.0, 0.0, -1.0)),
            glm::dvec3(0.0, 0.0, 1.0), viewport, Math::degreesToRadians(60.0),
            Math::degreesToRadians(40.0), Ellipsoid::WGS84);
    }

    // Cesium 的 maximumCachedBytes 只算 glTF buffer + 解码图像；我们的 DTO
    // （双精度顶点、RGBA8 贴图、射线索引）比它大一个数量级，必须单独记账。
    // 超预算时按实测比例下调模型缓存上限驱逐未使用的瓦片；若选中的瓦片本身
    // 就超预算，则像 CesiumJS 的 memoryAdjustedScreenSpaceError 一样渐进放大
    // SSE，宁可退回更粗层级，也不让常驻内存无界增长。
    void updateMemoryBudget(qint64 now)
    {
        if (!m_tileset || !m_renderer)
            return;
        // 3D Tiles 的内存由三块组成：Cesium 的 glTF 内容、我们的 DTO（双精度
        // 顶点 + RGBA8 贴图 + 射线索引）、以及 Renderer 的显存副本（≈场景 DTO）。
        // 配置里的 cacheBytes + overflowBytes 是整份数据的总上限，三块一起记。
        const std::uint64_t totalBudget = std::max<std::uint64_t>(
            64ull << 20, m_settings.cacheBytes + m_settings.overflowBytes);
        const std::uint64_t dto = m_renderer->dtoBytes();
        const std::uint64_t modelBytes = m_renderer->modelBytes();
        const std::uint64_t usedBytes = dto + modelBytes + m_sceneDtoBytes;
        Cesium3DTilesSelection::TilesetOptions& options = m_tileset->getOptions();
        // 缓存上限按"当前选中集合的模型大小 × 1.5 + 64 MB"给足前瞻量：
        // 选中的瓦片必须常驻，未选中的缓存只保留一屏左右的余量。这样 DTO
        // 总量不会被"看得见但用不到"的缓存占满，视野拉远后 SSE 也能回落。
        int64_t cacheTarget = m_initialModelCache;
        if (!m_selectedKeys.isEmpty())
            cacheTarget = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(m_selectedModelBytes) * 5 / 4 + (32ll << 20),
                32ll << 20, m_initialModelCache);
        if (usedBytes > totalBudget) {
            m_underBudgetSinceMs = 0;
            if (now - m_lastSseRaiseMs >= 100) {
                options.maximumScreenSpaceError = std::min(
                    m_maxSse, std::max(options.maximumScreenSpaceError, m_baseSse) * 1.05);
                m_lastSseRaiseMs = now;
            }
            // 超预算时优先挤掉缓存（不动选中集合），再靠 SSE 退层。
            if (now - m_lastCacheAdjustMs >= 250 &&
                cacheTarget < options.maximumCachedBytes) {
                options.maximumCachedBytes = cacheTarget;
                m_lastCacheAdjustMs = now;
            }
        } else {
            if (m_underBudgetSinceMs == 0)
                m_underBudgetSinceMs = now;
            if (usedBytes * 10ull < totalBudget * 9ull &&
                options.maximumScreenSpaceError > m_baseSse &&
                now - m_underBudgetSinceMs >= 1000 && now - m_lastSseLowerMs >= 150) {
                options.maximumScreenSpaceError =
                    std::max(m_baseSse, options.maximumScreenSpaceError / 1.05);
                m_lastSseLowerMs = now;
            }
            if (now - m_lastCacheAdjustMs >= 500) {
                if (cacheTarget < options.maximumCachedBytes) {
                    // 选中集合缩小/视野拉远：立即收紧缓存，释放 DTO。
                    options.maximumCachedBytes = cacheTarget;
                    m_lastCacheAdjustMs = now;
                } else if (usedBytes * 10ull < totalBudget * 9ull &&
                           cacheTarget > options.maximumCachedBytes) {
                    // 缓存只保留一屏左右的前瞻量，每次抬 32 MB；抬得太多
                    // 会立刻把 DTO 推回预算之上，形成"抬高→驱逐"的来回。
                    options.maximumCachedBytes = std::min<std::int64_t>(
                        cacheTarget, options.maximumCachedBytes + (32ll << 20));
                    m_lastCacheAdjustMs = now;
                }
            }
        }
        m_sseEffective = options.maximumScreenSpaceError;
    }

    void syncScene(const Cesium3DTilesSelection::ViewUpdateResult& update, qint64 now)
    {
        QHash<QString, SelectedTile> selected;
        for (const Cesium3DTilesSelection::Tile* tile : update.tilesToRenderThisFrame) {
            if (!tile)
                continue;
            const auto* content = tile->getContent().getRenderContent();
            if (!content)
                continue;
            const auto* resource =
                static_cast<const TileResource*>(content->getRenderResources());
            if (!resource || !resource->mesh || resource->mesh->vertices.empty() ||
                resource->mesh->primitives.empty() ||
                resource->mesh->hasContentOrigin != m_globeMode)
                continue;   // 模式不符：旧会话在途烘焙的瓦片不进当前场景
            QString key = resource->key;
            if (key.isEmpty())
                key = tileIdString(*tile);
            if (key.isEmpty())
                continue;
            selected.insert(key, SelectedTile { resource->mesh, resource->released,
                                                 resource->modelBytes });
        }

        bool changed = selected.size() != m_selectedKeys.size();
        if (!changed) {
            for (auto it = selected.constBegin(); it != selected.constEnd(); ++it) {
                if (!m_selectedKeys.contains(it.key())) {
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            m_selectedKeys = QSet<QString>();
            for (auto it = selected.constBegin(); it != selected.constEnd(); ++it)
                m_selectedKeys.insert(it.key());
            m_selectionChangedMs = now;
        }

        std::uint32_t readyCount = 0;
        m_selectedModelBytes = 0;
        for (auto it = selected.begin(); it != selected.end(); ++it) {
            m_selectedModelBytes += static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, it.value().modelBytes));
            const QString& key = it.key();
            const SelectedTile& source = it.value();
            SceneTile& tile = m_sceneTiles[key];
            if (tile.mesh && tile.mesh != source.mesh) {
                if (tile.visible)
                    m_scene.setTileVisible(m_groupKey, key, false);
                m_scene.removeTile(m_groupKey, key);
                m_sceneTiles.erase(m_sceneTiles.find(key));
                continue;
            }
            if (!tile.mesh) {
                if (!m_scene.addTile(m_groupKey, key, source.mesh)) {
                    ++m_addFailures;
                    continue;
                }
                tile.mesh = source.mesh;
                tile.released = source.released;
                tile.modelBytes = source.modelBytes;
                tile.visible = false;
                m_scene.setTileVisible(m_groupKey, key, false);
            }
            tile.lastSelectedMs = now;
            if (m_scene.tileRenderState(m_groupKey, key) ==
                View3D::LayerRenderState::Ready) {
                ++readyCount;
                if (!tile.visible) {
                    m_scene.setTileVisible(m_groupKey, key, true);
                    tile.visible = true;
                }
            }
        }

        const bool allReady =
            !selected.isEmpty() && readyCount == static_cast<std::uint32_t>(selected.size());
        const bool forceSwap =
            !selected.isEmpty() && now - m_selectionChangedMs > 10000;
        if (allReady || forceSwap) {
            for (auto it = m_sceneTiles.begin(); it != m_sceneTiles.end(); ++it) {
                if (selected.contains(it.key()))
                    continue;
                if (it->visible) {
                    m_scene.setTileVisible(m_groupKey, it.key(), false);
                    it->visible = false;
                }
            }
        }

        for (auto it = m_sceneTiles.begin(); it != m_sceneTiles.end();) {
            if (selected.contains(it.key()) || it->visible) {
                ++it;
                continue;
            }
            const bool released =
                it->released && it->released->load();
            if (!released && now - it->lastSelectedMs <= 3000) {
                ++it;
                continue;
            }
            m_scene.removeTile(m_groupKey, it.key());
            it = m_sceneTiles.erase(it);
        }

        m_selectedVisible = 0;
        m_sceneDtoBytes = 0;
        for (const SceneTile& tile : m_sceneTiles) {
            if (tile.visible)
                ++m_selectedVisible;
            if (tile.mesh)
                m_sceneDtoBytes += View3D::modelMeshCpuBytes(*tile.mesh);
        }
        m_residentTiles = m_scene.tileCount(m_groupKey);
        m_visibleTiles = m_selectedVisible;

        if (!m_framed) {
            frameGroup(now);
        }
    }

    void frameGroup(qint64 now)
    {
        if (!m_layout) {
            m_framed = true;
            return;
        }
        if (now - m_startMs > 10000) {
            m_framed = true;
            std::cerr << "VIEW3D_TILES_FRAME_SKIPPED group=" << m_groupKey.toStdString()
                      << std::endl;
            return;
        }
        if (m_residentTiles == 0)
            return;
        QList<View3D::IRenderSurface*> views = m_layout->views();
        if (views.isEmpty()) {
            if (View3D::IRenderSurface* active = m_layout->activeView())
                views.append(active);
            else if (View3D::IRenderSurface* primary = m_layout->primaryView())
                views.append(primary);
        }
        bool focused = false;
        for (View3D::IRenderSurface* view : views) {
            if (view && view->focusTileGroup(m_groupKey))
                focused = true;
        }
        if (!focused)
            return;
        m_framed = true;
        std::cerr << "VIEW3D_TILES_FRAMED group=" << m_groupKey.toStdString()
                  << " ms=" << (now - m_startMs) << std::endl;
    }

    void logProgress(qint64 now)
    {
        if (now - m_lastLogMs < 2000)
            return;
        m_lastLogMs = now;
        const TilesetRendererResources::Snapshot snapshot = m_renderer
            ? m_renderer->snapshot()
            : TilesetRendererResources::Snapshot {};
        const std::uint64_t requests = m_accessor ? m_accessor->requestCount() : 0;
        // 缺文件必须显式计数：内容 404 不会触发 TilesetLoadFailureDetails，
        // 只会在画面上留下补不齐的空洞，不记出来就完全看不见。
        const std::uint64_t missing = m_accessor ? m_accessor->missingCount() : 0;
        std::cerr << "VIEW3D_TILES_STREAM group=" << m_groupKey.toStdString()
                  << " resident=" << m_residentTiles << " visible=" << m_visibleTiles
                  << " selected=" << m_selectedKeys.size()
                  << " prepared=" << snapshot.prepared
                  << " main_ready=" << snapshot.mainPrepared
                  << " freed=" << snapshot.freed << " live=" << snapshot.live
                  << " requests=" << requests
                  << " missing=" << missing
                  << " vertices=" << snapshot.conversion.vertices
                  << " primitives=" << snapshot.conversion.primitives
                  << " textures=" << snapshot.conversion.textures
                  << " missing_normals=" << snapshot.conversion.missingNormals
                  << " skipped=" << snapshot.conversion.skippedPrimitives
                  << " add_failures=" << m_addFailures
                  << " cesium_mb=" << (snapshot.dtoBytes / (1024.0 * 1024.0))
                  << " model_mb=" << (snapshot.modelBytes / (1024.0 * 1024.0))
                  << " selected_model_mb=" << (m_selectedModelBytes / (1024.0 * 1024.0))
                  << " scene_mb=" << (m_sceneDtoBytes / (1024.0 * 1024.0))
                  << " sse_eff=" << m_sseEffective
                  << " budget_mb=" << ((m_settings.cacheBytes + m_settings.overflowBytes) /
                                       (1024.0 * 1024.0))
                  << " cache_mb=" << (m_tileset
                          ? m_tileset->getOptions().maximumCachedBytes / (1024.0 * 1024.0)
                          : 0.0)
                  << " framed=" << (m_framed ? 1 : 0) << std::endl;
    }

    View3D::IRenderScene& m_scene;
    View3D::IViewportLayout* m_layout = nullptr;
    TilesetWorldFramePtr m_frame;
    QString m_path;
    QString m_groupKey;
    QString m_displayName;
    View3D::TileCacheSettings m_settings;
    TilesetConvertOptions m_convertOptions;
    std::shared_ptr<TilesetTaskProcessor> m_tasks;
    CesiumAsync::AsyncSystem m_asyncSystem;
    std::shared_ptr<TilesetAssetAccessor> m_accessor;
    std::shared_ptr<TilesetRendererResources> m_renderer;
    std::unique_ptr<Cesium3DTilesSelection::Tileset> m_tileset;
    QTimer m_tick;
    QObject m_context;
    QHash<QString, SceneTile> m_sceneTiles;
    QSet<QString> m_selectedKeys;
    qint64 m_startMs = 0;
    qint64 m_lastUpdateMs = 0;
    qint64 m_lastLogMs = 0;
    qint64 m_selectionChangedMs = 0;
    std::uint32_t m_residentTiles = 0;
    std::uint32_t m_visibleTiles = 0;
    std::uint32_t m_selectedVisible = 0;
    std::uint32_t m_addFailures = 0;
    bool m_framed = false;
    bool m_rooted = false;
    bool m_globeMode = false;
    // 框架回调只报一次：图层属性拿到矩阵后不必每帧重填。
    WorldFrameCallback m_frameCallback;
    bool m_frameReported = false;
    double m_baseSse = 16.0;
    double m_maxSse = 512.0;
    double m_sseEffective = 16.0;
    int64_t m_initialModelCache = 512ll << 20;
    qint64 m_lastSseRaiseMs = 0;
    qint64 m_lastSseLowerMs = 0;
    qint64 m_lastCacheAdjustMs = 0;
    qint64 m_underBudgetSinceMs = 0;
    std::uint64_t m_selectedModelBytes = 0;
    std::uint64_t m_sceneDtoBytes = 0;
    QString m_error;
};

TilesetStreaming::TilesetStreaming(View3D::IRenderScene& scene,
                                   View3D::IViewportLayout* layout,
                                   TilesetWorldFramePtr frame, QString tilesetPath,
                                   QString groupKey, QString displayName,
                                   View3D::TileCacheSettings settings)
    : m_impl(std::make_unique<Impl>(scene, layout, std::move(frame),
                                    std::move(tilesetPath), std::move(groupKey),
                                    std::move(displayName), settings)),
      m_groupKey(m_impl->m_groupKey),
      m_tilesetPath(m_impl->m_path),
      m_displayName(m_impl->m_displayName)
{
}

TilesetStreaming::~TilesetStreaming()
{
    if (m_impl)
        m_impl->stop();
}

bool TilesetStreaming::start()
{
    if (!m_impl)
        return false;
    try {
        return m_impl->start();
    } catch (const std::exception& error) {
        m_impl->m_error = QStringLiteral("启动 3D Tiles 会话异常：%1")
                              .arg(QString::fromUtf8(error.what()));
    } catch (...) {
        m_impl->m_error = QStringLiteral("启动 3D Tiles 会话异常");
    }
    std::cerr << "VIEW3D_TILES_START_EXCEPTION group=" << m_groupKey.toStdString()
              << " reason=" << m_impl->m_error.toStdString() << std::endl;
    return false;
}

void TilesetStreaming::stop()
{
    if (!m_impl)
        return;
    try {
        m_impl->stop();
    } catch (...) {
        std::cerr << "VIEW3D_TILES_STOP_EXCEPTION group=" << m_groupKey.toStdString()
                  << std::endl;
    }
}

void TilesetStreaming::detachFromScene()
{
    if (!m_impl)
        return;
    try {
        m_impl->detachFromScene();
    } catch (...) {
    }
}

void TilesetStreaming::setGlobeMode(bool enabled)
{
    if (m_impl)
        m_impl->m_globeMode = enabled;
}

bool TilesetStreaming::globeMode() const noexcept
{
    return m_impl && m_impl->m_globeMode;
}

void TilesetStreaming::keepCameraOnRestart()
{
    if (m_impl)
        m_impl->m_framed = true;
}

void TilesetStreaming::setWorldFrameCallback(WorldFrameCallback callback)
{
    if (!m_impl)
        return;
    m_impl->m_frameCallback = std::move(callback);
}

std::uint32_t TilesetStreaming::residentTiles() const noexcept
{
    return m_impl ? m_impl->m_residentTiles : 0;
}

std::uint32_t TilesetStreaming::visibleTiles() const noexcept
{
    return m_impl ? m_impl->m_visibleTiles : 0;
}

std::uint64_t TilesetStreaming::loadedTiles() const noexcept
{
    if (!m_impl || !m_impl->m_renderer)
        return 0;
    return m_impl->m_renderer->snapshot().prepared;
}

std::uint64_t TilesetStreaming::requests() const noexcept
{
    return m_impl && m_impl->m_accessor ? m_impl->m_accessor->requestCount() : 0;
}

bool TilesetStreaming::rooted() const noexcept
{
    return m_impl && m_impl->m_rooted;
}

QString TilesetStreaming::lastError() const
{
    return m_impl ? m_impl->m_error : QString();
}

} // namespace View3DTiles
