#include "OsgbStreaming.h"

#include <GlobeMath.h>
#include <IGeoReference.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <ISceneContent.h>
#include <IViewportCamera.h>
#include <IViewportLayout.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

const QRegularExpression& levelPattern()
{
    static const QRegularExpression pattern(QStringLiteral("_L(\\d+)"));
    return pattern;
}

int levelOf(const QString& name)
{
    const QRegularExpressionMatch match = levelPattern().match(name);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// 场景会保留 DTO 供拾取与重传，同时再在显存里放一份：贴图按含 mip 的 4/3 计，
// 顶点按 pos+normal+uv 的 32 字节，索引 4 字节，数据块常量约 512 字节。
// 图集跨瓦片去重时这里偏保守（共享贴图被重复计入），宁可提前淘汰。
std::size_t estimateRetainedBytes(const View3DOsb::ConvertedTile& converted)
{
    std::uint64_t bytes = converted.cpuBytes;
    if (converted.mesh) {
        for (const View3D::ModelTextureDto& texture : converted.mesh->textures) {
            bytes += static_cast<std::uint64_t>(texture.width) *
                static_cast<std::uint64_t>(std::max(texture.height, 1)) * 4u * 4u / 3u;
        }
        bytes += static_cast<std::uint64_t>(converted.mesh->vertices.size()) * 32u;
        bytes += static_cast<std::uint64_t>(converted.mesh->indices.size()) * 4u;
        bytes += static_cast<std::uint64_t>(converted.mesh->primitives.size()) * 512u;
    }
    return static_cast<std::size_t>(bytes);
}

} // namespace

namespace View3DOsb {

OsgbStreaming::OsgbStreaming(View3D::IRenderScene& scene, View3D::IViewportLayout* layout,
                             DatasetDescriptor descriptor, View3D::TileCacheSettings settings,
                             View3D::IGeoReference* reference)
    : m_scene(scene), m_layout(layout), m_descriptor(std::move(descriptor)),
      m_settings(settings), m_reference(reference)
{
    m_settings.clampToValidRange();
    m_refinePixels = m_settings.screenSpaceErrorPixels;
    m_keepParents = qEnvironmentVariableIsSet("VIEW3D_OSBG_KEEP_PARENTS");
    m_meterSlotsOnly = qEnvironmentVariableIsSet("VIEW3D_OSBG_METER_SLOTS");
    m_legacyReady = qEnvironmentVariableIsSet("VIEW3D_OSBG_LEGACY_READY");
    m_groupKey = QStringLiteral("osgb:") + QFileInfo(m_descriptor.directory).fileName();
    m_tick.setInterval(40);
    QObject::connect(&m_tick, &QTimer::timeout, &m_context, [this] { tick(); });
}

OsgbStreaming::~OsgbStreaming()
{
    stop();
}

void OsgbStreaming::resolveGlobeOrigin()
{
    using namespace View3D::GlobeMath;
    m_globeOriginGeographic = {0.0, 0.0, 0.0};
    if (!m_descriptor.srs.trimmed().isEmpty() && m_reference) {
        std::array<double, 3> origin = m_descriptor.origin;
        if (m_reference->transformToMain(m_descriptor.srs.trimmed(), origin.data(), 1))
            m_globeOriginGeographic = origin;
        else
            std::cerr << "VIEW3D_OSBG_GLOBE_ORIGIN_FAILED srs="
                      << m_descriptor.srs.toStdString() << " origin=" << origin[0] << ","
                      << origin[1] << "," << origin[2] << std::endl;
    }
    const double lon = m_globeOriginGeographic[0] * kRadiansPerDegree;
    const double lat = m_globeOriginGeographic[1] * kRadiansPerDegree;
    const double sinLon = std::sin(lon);
    const double cosLon = std::cos(lon);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    m_globeEast = { -sinLon, cosLon, 0.0 };
    m_globeNorth = { -sinLat * cosLon, -sinLat * sinLon, cosLat };
    m_globeUp = { cosLat * cosLon, cosLat * sinLon, sinLat };
    m_globeOriginEcef = geodeticToEcef(m_globeOriginGeographic[0],
                                       m_globeOriginGeographic[1],
                                       m_globeOriginGeographic[2]);
    m_globeOriginResolved = true;
}

void OsgbStreaming::globeCameraToLocal(View3D::ViewportCameraState* state) const
{
    if (!state || !m_globeOriginResolved)
        return;
    using namespace View3D::GlobeMath;
    const auto toLocalPoint = [this](const Vec3& value) {
        const Vec3 delta = sub(value, m_globeOriginEcef);
        return Vec3 { dot(delta, m_globeEast), dot(delta, m_globeUp),
                      -dot(delta, m_globeNorth) };
    };
    const auto toLocalVector = [this](const Vec3& value) {
        return Vec3 { dot(value, m_globeEast), dot(value, m_globeUp),
                      -dot(value, m_globeNorth) };
    };
    state->position = toLocalPoint(state->position);
    state->target = toLocalPoint(state->target);
    state->direction = normalized(toLocalVector(state->direction));
    state->up = normalized(toLocalVector(state->up));
}

bool OsgbStreaming::bakeTileToGlobe(ConvertedTile& tile) const
{
    if (!tile.mesh || tile.mesh->vertices.empty())
        return true;   // 空瓦片（只有 LOD 索引）无需烘焙
    using namespace View3D::GlobeMath;
    View3D::ModelMeshDto& mesh = *tile.mesh;
    const std::size_t count = mesh.vertices.size();
    const bool geographic = !m_descriptor.srs.trimmed().isEmpty() && m_reference;
    std::vector<Vec3> ecef(count);
    std::vector<double> source;
    if (geographic) {
        // 局部 (E,U,−N) 还原成源网格 (E,N,U) 再加回 SRSOrigin，逐点转经纬度。
        source.resize(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            const View3D::MeshVertexDto& vertex = mesh.vertices[i];
            source[i * 3] = m_descriptor.origin[0] + vertex.x;
            source[i * 3 + 1] = m_descriptor.origin[1] - vertex.z;
            source[i * 3 + 2] = m_descriptor.origin[2] + vertex.y;
        }
        if (!m_reference->transformToMain(m_descriptor.srs.trimmed(), source.data(), count)) {
            tile.error = QStringLiteral("球面烘焙失败：源坐标无法换算经纬度");
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const double lon = source[i * 3];
            const double lat = source[i * 3 + 1];
            const double height = source[i * 3 + 2];
            if (!std::isfinite(lon) || !std::isfinite(lat) || !std::isfinite(height) ||
                std::abs(lon) > 180.0 || std::abs(lat) > 90.0) {
                tile.error = QStringLiteral("球面烘焙失败：经纬度超出范围");
                return false;
            }
            ecef[i] = geodeticToEcef(lon, lat, height);
        }
    } else {
        // 成果没写参照系：局部原点钉在 (0°,0°) 地表，按当地 ENU 摆放。
        // 局部轴 (E,U,−N) 在该点的 ENU 基就是 (0,1,0)/(1,0,0)/(0,0,1)。
        const Vec3 anchor = geodeticToEcef(0.0, 0.0, 0.0);
        for (std::size_t i = 0; i < count; ++i) {
            const View3D::MeshVertexDto& vertex = mesh.vertices[i];
            ecef[i] = { anchor[0] + vertex.y, anchor[1] + vertex.x,
                        anchor[2] - vertex.z };
        }
    }
    Vec3 minimum = ecef.front();
    Vec3 maximum = ecef.front();
    for (const Vec3& point : ecef) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], point[axis]);
            maximum[axis] = std::max(maximum[axis], point[axis]);
        }
    }
    const Vec3 center = { 0.5 * (minimum[0] + maximum[0]),
                          0.5 * (minimum[1] + maximum[1]),
                          0.5 * (minimum[2] + maximum[2]) };
    mesh.hasContentOrigin = true;
    mesh.contentOrigin = center;
    for (std::size_t i = 0; i < count; ++i) {
        View3D::MeshVertexDto& vertex = mesh.vertices[i];
        const Vec3 offset = sub(ecef[i], center);
        vertex.x = offset[0];
        vertex.y = offset[1];
        vertex.z = offset[2];
        if (!vertex.hasNormal)
            continue;
        Vec3 east { 0.0, 1.0, 0.0 };
        Vec3 north { 0.0, 0.0, 1.0 };
        Vec3 up { 1.0, 0.0, 0.0 };
        if (geographic) {
            const double lonRad = source[i * 3] * kRadiansPerDegree;
            const double latRad = source[i * 3 + 1] * kRadiansPerDegree;
            const double sinLon = std::sin(lonRad);
            const double cosLon = std::cos(lonRad);
            const double sinLat = std::sin(latRad);
            const double cosLat = std::cos(latRad);
            east = { -sinLon, cosLon, 0.0 };
            north = { -sinLat * cosLon, -sinLat * sinLon, cosLat };
            up = { cosLat * cosLon, cosLat * sinLon, sinLat };
        }
        // 局部法线 (x=E, y=U, z=−N) → ECEF；比例项归一化后写回。
        const Vec3 rotated = add(add(scale(east, vertex.nx), scale(up, vertex.ny)),
                                 scale(north, -vertex.nz));
        const Vec3 unit = normalized(rotated);
        if (length(unit) > 0.5) {
            vertex.nx = unit[0];
            vertex.ny = unit[1];
            vertex.nz = unit[2];
        }
    }
    return true;
}

bool OsgbStreaming::start()
{
    if (m_descriptor.rootTileFiles.isEmpty())
        return false;
    m_lastError.clear();
    m_rootKeys.clear();
    if (m_globeMode)
        resolveGlobeOrigin();
    for (const QString& file : m_descriptor.rootTileFiles)
        m_rootKeys.append(ensureTile(tileKeyFor(file), file, 0).key);

    // 多数 OSGB 成果已是相对 SRSOrigin 的局部坐标；若是绝对坐标就减掉原点，
    // 否则单精度顶点在千米级偏移上会抖。
    bool originIsZero = true;
    const auto localEnough = [](const ConvertedTile& converted) {
        if (!converted.hasBounds)
            return true;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (std::abs(converted.boundsMaximum[axis]) >= 1.0e5 ||
                std::abs(converted.boundsMinimum[axis]) >= 1.0e5)
                return false;
        }
        return true;
    };

    // 根瓦片同步读取，首屏立刻有内容；更深的层级全部走工作线程流式加载。
    for (const QString& rootKey : std::as_const(m_rootKeys)) {
        Tile& tile = *m_tiles.at(rootKey);
        ConvertedTile converted;
        if (!convertTileFile(tile.path, m_origin, &converted)) {
            // 只留第一条：根瓦片全读不出来时，弹窗/日志给出的就是最原始的那个原因。
            if (m_lastError.isEmpty())
                m_lastError = converted.error.isEmpty()
                    ? QStringLiteral("读不出来：%1").arg(tile.path)
                    : converted.error;
            continue;
        }
        if (originIsZero && !localEnough(converted)) {
            originIsZero = false;
            m_origin = m_descriptor.origin;
            ConvertedTile rebased;
            if (convertTileFile(tile.path, m_origin, &rebased) && rebased.valid())
                converted = std::move(rebased);
            std::cerr << "VIEW3D_OSBG_REBASE origin=" << m_origin[0] << "," << m_origin[1]
                      << "," << m_origin[2] << std::endl;
        }
        if (m_globeMode && !bakeTileToGlobe(converted)) {
            if (m_lastError.isEmpty())
                m_lastError = converted.error.isEmpty()
                    ? QStringLiteral("球面烘焙失败：%1").arg(tile.path)
                    : converted.error;
            continue;
        }
        adoptConverted(rootKey, std::move(converted));
    }
    if (m_residentTiles == 0) {
        if (m_lastError.isEmpty()) {
            m_lastError = QStringLiteral("所有根瓦片都没能读出几何（共 %1 个）")
                              .arg(m_descriptor.rootTileFiles.size());
        }
        return false;
    }
    m_startMs = nowMs();
    m_lastLogMs = m_startMs;
    m_tick.start();
    return true;
}

void OsgbStreaming::stop()
{
    m_tick.stop();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_stopping = true;
        for (std::deque<Job>& queue : m_queues)
            queue.clear();
    }
    m_jobCondition.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }
    m_workers.clear();
    m_alive->store(false);
}

OsgbStreaming::Tile& OsgbStreaming::ensureTile(const QString& key, const QString& path,
                                               int level)
{
    const auto found = m_tiles.find(key);
    if (found != m_tiles.end())
        return *found->second;
    auto tile = std::make_unique<Tile>();
    tile->key = key;
    tile->path = path;
    tile->level = level;
    tile->lastIndexMs = nowMs();
    // 骨架自身的开销：两条字符串加上容器与定长字段。
    tile->indexBytes = 256 + static_cast<std::size_t>(key.size() + path.size()) * 2u;
    m_indexBytes += tile->indexBytes;
    Tile& reference = *tile;
    m_tiles.emplace(key, std::move(tile));
    return reference;
}

OsgbStreaming::Tile* OsgbStreaming::tileAt(const QString& key) noexcept
{
    const auto found = m_tiles.find(key);
    return found == m_tiles.end() ? nullptr : found->second.get();
}

// 键早先取文件基础名，两份成果撞在一起就会出事：有些成果的根瓦片不叫 <目录名>.osgb，
// 而是一个目录一个 Model.osgb / Block.osgb（旧版读取器 ci_osgb_reader 的候选表里就有），
// 那时两块完全不同的瓦片会被并成同一条 m_tiles 记录，第二块直接拿到第一块的几何，
// 而且永远加载不回来。改成相对瓦片目录的路径：仍是路径的纯函数（同一个子瓦片被多个
// 父级引用时照样命中同一条记录），但对不同文件一定不同；相对路径也避免键变得过长，
// 以及成果被整体移动后键跟着变。
QString OsgbStreaming::tileKeyFor(const QString& path) const
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString base = QDir::cleanPath(m_descriptor.dataDirectory);
    if (!base.isEmpty() && cleaned.size() > base.size() + 1 &&
        cleaned.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive))
        return cleaned.mid(base.size() + 1);
    return cleaned;
}

void OsgbStreaming::lruUnlink(Tile& tile)
{
    if (tile.lruList < 0 || tile.lruList >= kLruCount)
        return;
    LruHead& head = m_lru[tile.lruList];
    if (tile.lruPrev)
        tile.lruPrev->lruNext = tile.lruNext;
    else
        head.head = tile.lruNext;
    if (tile.lruNext)
        tile.lruNext->lruPrev = tile.lruPrev;
    else
        head.tail = tile.lruPrev;
    tile.lruPrev = nullptr;
    tile.lruNext = nullptr;
    if (head.count > 0)
        --head.count;
    tile.lruList = -1;
}

void OsgbStreaming::lruPushFront(int list, Tile& tile)
{
    if (tile.lruList == list && tile.lruPrev == nullptr)
        return;
    lruUnlink(tile);
    LruHead& head = m_lru[list];
    tile.lruPrev = nullptr;
    tile.lruNext = head.head;
    if (head.head)
        head.head->lruPrev = &tile;
    else
        head.tail = &tile;
    head.head = &tile;
    ++head.count;
    tile.lruList = list;
}

// 上过屏的瓦片移到"最近渲染"链头；没上屏的留在"最近更新"链里，优先被丢弃。
void OsgbStreaming::lruTouch(Tile& tile)
{
    lruUnlink(tile);
    lruPushFront(LruRendered, tile);
}

bool OsgbStreaming::pickCamera(CameraFrame* camera) const
{
    if (!m_layout || !camera)
        return false;
    View3D::IRenderSurface* view = m_layout->activeView();
    if (!view)
        view = m_layout->primaryView();
    if (!view)
        return false;
    View3D::ViewportCameraState state;
    if (!view->cameraState(&state))
        return false;
    // 球面：相机状态是 ECEF，先换到数据集局部系；后面的包围球/视锥/DSE
    // 公式全部沿用本地坐标（瓦片包围盒从未离开数据集局部坐标）。
    if (m_globeMode)
        globeCameraToLocal(&state);
    camera->position = state.position;
    camera->orthographic = state.orthographic;
    const double heightPixels = state.viewport[1] > 1.0 ? state.viewport[1] : 720.0;
    const double vertical =
        state.verticalFovRadians > 0.01 ? state.verticalFovRadians : 0.7853982;
    if (state.orthographic && state.orthographicHeight > 0.01) {
        camera->pixelsPerUnit = heightPixels / state.orthographicHeight;
    } else {
        camera->pixelsPerRadian = heightPixels / (2.0 * std::tan(vertical * 0.5));
    }
    std::array<double, 3> direction = state.direction;
    if (direction[0] * direction[0] + direction[1] * direction[1] +
            direction[2] * direction[2] <
        1e-9) {
        direction = {state.target[0] - state.position[0],
                     state.target[1] - state.position[1],
                     state.target[2] - state.position[2]};
    }
    double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                              direction[2] * direction[2]);
    if (length < 1e-9) {
        direction = {0.0, 0.0, -1.0};
        length = 1.0;
    }
    camera->direction = {direction[0] / length, direction[1] / length, direction[2] / length};
    std::array<double, 3> up = state.up;
    double upLength =
        std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (upLength < 1e-9) {
        up = {0.0, 1.0, 0.0};
        upLength = 1.0;
    }
    for (double& value : up)
        value /= upLength;
    std::array<double, 3> right {
        camera->direction[1] * up[2] - camera->direction[2] * up[1],
        camera->direction[2] * up[0] - camera->direction[0] * up[2],
        camera->direction[0] * up[1] - camera->direction[1] * up[0]};
    double rightLength =
        std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rightLength < 1e-6) {
        // 视线与 up 平行：换一个参考轴重建右手基，避免退化成零向量。
        right = {camera->direction[1] * 1.0 - camera->direction[2] * 0.0,
                 camera->direction[2] * 0.0 - camera->direction[0] * 1.0,
                 camera->direction[0] * 0.0 - camera->direction[1] * 0.0};
        rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                right[2] * right[2]);
        if (rightLength < 1e-6) {
            right = {1.0, 0.0, 0.0};
            rightLength = 1.0;
        }
    }
    for (double& value : right)
        value /= rightLength;
    camera->right = right;
    camera->up = {right[1] * camera->direction[2] - right[2] * camera->direction[1],
                  right[2] * camera->direction[0] - right[0] * camera->direction[2],
                  right[0] * camera->direction[1] - right[1] * camera->direction[0]};
    const double aspect =
        state.viewport[0] > 1.0 && state.viewport[1] > 1.0
            ? state.viewport[0] / state.viewport[1]
            : 4.0 / 3.0;
    const double horizontal =
        state.horizontalFovRadians > 0.01
            ? state.horizontalFovRadians
            : 2.0 * std::atan(std::tan(vertical * 0.5) * aspect);
    // 视锥放宽 7 度：够覆盖正常转头的提前量。之前放宽 20 度，把大量屏外边缘瓦片
    // 也算进需求，它们占住预算又受"替补"保护，屏内的细瓦片反而挤不进来。
    const double margin = 0.12;
    const double halfVertical = std::min(vertical * 0.5 + margin, 1.5);
    const double halfHorizontal = std::min(horizontal * 0.5 + margin, 1.5);
    camera->cosVertical = std::cos(halfVertical);
    camera->sinVertical = std::sin(halfVertical);
    camera->cosHorizontal = std::cos(halfHorizontal);
    camera->sinHorizontal = std::sin(halfHorizontal);
    // 中心优先锥：屏幕中心这块区域的需求排最高优先级，先把看得清楚的地方补细。
    camera->centerCosine =
        std::cos(std::max(vertical, horizontal) * 0.5 * m_settings.centerConeFactor);
    return true;
}

// 单个 PagedLOD 节点的几何评估。两个遍历共用，避免两份公式漂移。
// SSE 用包围球最近点距离（Cesium 口径）；dynamicScreenSpaceError 按逐瓦片距离
// 从误差里减去，而不是整体放大阈值；距离槽按成果声明的 RangeMode 选比较量。
OsgbStreaming::NodeView OsgbStreaming::evaluateNode(const LodNode& node,
                                                    const CameraFrame& camera,
                                                    double threshold) const
{
    NodeView view;
    const std::array<double, 3>& eye = camera.position;
    const std::array<double, 3>& dir = camera.direction;
    const double dx = node.center[0] - eye[0];
    const double dy = node.center[1] - eye[1];
    const double dz = node.center[2] - eye[2];
    view.toCentre = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Cesium 距离取包围体最近点，相机在球内时夹到极小值而不是 1 m 地板。
    view.distance = std::max(view.toCentre - node.radius, 1.0e-7);
    const double along = dx * dir[0] + dy * dir[1] + dz * dir[2];
    view.cosAngle = view.toCentre > 1e-6 ? along / view.toCentre : 1.0;
    // 球-视锥相交：球心到四个侧面的有符号距离都不超过半径。早先
    // "along <= 0 一律算可见"会把相机背后的整个半球都算进需求，白占预算。
    if (along + node.radius < 0.0) {
        view.inView = false;
    } else {
        const double x = dx * camera.right[0] + dy * camera.right[1] + dz * camera.right[2];
        const double y = dx * camera.up[0] + dy * camera.up[1] + dz * camera.up[2];
        const double radius = node.radius;
        view.inView =
            x * camera.cosHorizontal - along * camera.sinHorizontal <= radius &&
            -x * camera.cosHorizontal - along * camera.sinHorizontal <= radius &&
            y * camera.cosVertical - along * camera.sinVertical <= radius &&
            -y * camera.cosVertical - along * camera.sinVertical <= radius;
    }
    double projected = camera.orthographic
        ? node.radius * camera.pixelsPerUnit
        : node.radius / view.distance * camera.pixelsPerRadian;
    if (camera.dseDensity > 0.0 && m_settings.dseFactor > 0.0) {
        const double scaled = view.distance * camera.dseDensity;
        projected -= (1.0 - std::exp(-(scaled * scaled))) * m_settings.dseFactor;
    }
    view.error = projected;
    view.refine = projected >= threshold;
    // 距离槽的比较量必须和成果书写 range 时用的单位一致（见 osg::LOD::traverse）：
    //   DISTANCE_FROM_EYE_POINT → 到 LOD 中心的球心距离（米）
    //   PIXEL_SIZE_ON_SCREEN    → 包围球在屏上的投影像素尺寸，OSG 的口径是
    //                             radius/(球心的视深)，分母是垂直于视平面的深度
    //                             而不是斜距，也不是包围球最近点距离。
    // 拿米去比像素阈值时，阈值只有十几到几十"米"，等于要求相机钻进节点内部才继续
    // 下钻，靠近反而退回本文件的粗几何。
    double rangeValue = view.toCentre;
    if (node.pixelSizeMode && !m_meterSlotsOnly) {
        const double viewDepth = std::max(std::abs(along), 1.0e-7);
        rangeValue = camera.orthographic ? node.radius * camera.pixelsPerUnit
                                        : node.radius / viewDepth * camera.pixelsPerRadian;
    }
    for (std::size_t index = 0; index < node.rangeSlots.size(); ++index) {
        const LodSlot& slot = node.rangeSlots[index];
        if (rangeValue < slot.minimum || rangeValue >= slot.maximum)
            continue;
        if (slot.external)
            view.externalSlots |= 1ull << std::min<std::size_t>(index, 63);
        else if (index > 0)
            view.hasInlineSlot = true; // 槽 0 就是本文件内容，已作为自身几何保留
    }
    return view;
}

// 第一遍：只决定"该下载谁"。按索引自由下钻——内容被淘汰但骨架还在的中间层
// 也必须继续往下派发需求，否则整棵子树被冻结（表现为近处一直不刷新）。
void OsgbStreaming::propagate(Tile& tile, const CameraFrame& camera, int depth)
{
    if (depth > kMaxLodDepth || tile.demandPass == m_passSerial)
        return;
    tile.demandPass = m_passSerial;
    tile.lastIndexMs = nowMs(); // 只算"骨架被用到"，不代表内容还要留着
    for (const LodNode& node : tile.nodes) {
        const NodeView view = evaluateNode(node, camera, m_refinePixels);
        if (!view.inView) {
            // 兄弟预取：只在视野内需求已排空、没越过硬上限、且缓存确实有空位时排队。
            // 缓存满时预取会形成"预取→淘汰→再预取"的死循环：预取的瓦片从未上屏，
            // 是第一批被淘汰的，实测静止放大时 95% 的加载都是这种无效功。
            if (m_settings.preloadSiblings && view.refine && !m_hardOverflow &&
                m_prefetchAllowed && m_pendingDemand == 0u) {
                for (std::size_t index = 0; index < node.rangeSlots.size(); ++index) {
                    if ((view.externalSlots &
                         (1ull << std::min<std::size_t>(index, 63))) == 0)
                        continue;
                    const LodSlot& slot = node.rangeSlots[index];
                    Tile& sibling =
                        ensureTile(slot.childKey, slot.childFile, levelOf(slot.childKey));
                    sibling.prefetch = true;
                    sibling.demandDistance = std::min(sibling.demandDistance, view.distance);
                    sibling.demandAngle = std::acos(std::clamp(view.cosAngle, -1.0, 1.0));
                }
            }
            continue; // 不在屏上：本文件隐藏也看不见，不必为它保留父级
        }
        if (!view.refine || view.externalSlots == 0)
            continue; // 该 LOD 节点在本距离就用本文件内容，无需更细
        // 同一节点命中的外部子文件全部要请求：OSG PagedLOD 允许多个子节点
        // 在同一距离同时显示，只取第一个会让其余区域永远停在父级（或露洞）。
        for (std::size_t index = 0; index < node.rangeSlots.size(); ++index) {
            if ((view.externalSlots & (1ull << std::min<std::size_t>(index, 63))) == 0)
                continue;
            const LodSlot& slot = node.rangeSlots[index];
            Tile& child = ensureTile(slot.childKey, slot.childFile, levelOf(slot.childKey));
            if (child.demanded)
                ++m_sharedChildEdges; // 同一块子瓦片被多个节点引用
            child.prefetch = false;
            child.demanded = true;
            child.inCenterCone = child.inCenterCone || view.cosAngle >= camera.centerCosine;
            // 紧迫度 = 这一级超出误差阈值的倍数。近处物体超得多，排前面；
            // 只按角度或距离排会让屏幕中心的天际线远处抢走带宽。
            const double urgency =
                std::max(0.0, view.error) / std::max(m_refinePixels, 1.0);
            if (view.distance < child.demandDistance) {
                child.demandDistance = view.distance;
                child.demandAngle = std::acos(std::clamp(view.cosAngle, -1.0, 1.0));
            }
            child.demandUrgency = std::max(child.demandUrgency, urgency);
            if (child.resident || !child.nodes.empty())
                propagate(child, camera, depth + 1);
        }
    }
}

// 第二遍：只决定"该显示谁"。替换单位是 PagedLOD 节点而不是整个文件：
// 同一文件里已被子瓦片接管的组隐藏自身几何，仍在用本级的组继续显示，
// 不会因为一个组没就位就把整份文件退回粗层（那样会让已就绪的细瓦片白白消失）。
// shownOut 只追加"图层可见"的条目；条目内 nodeMask 为 0 表示只显示文件自身几何。
bool OsgbStreaming::selectShown(Tile& tile, const CameraFrame& camera, int depth,
                                std::vector<ShownEntry>& shownOut)
{
    // 遍历链上的每一级都算"本轮用得上"：它们可能自己不显示，但子树要靠它们下来。
    tile.demanded = true;
    if (depth > kMaxLodDepth) {
        if (!tile.displayable)
            return false;
        shownOut.push_back({&tile, nodeMaskAll(tile.nodes.size())});
        return true;
    }
    std::uint64_t selfMask = 0;
    std::uint64_t replacedMask = 0;
    bool childShown = false;
    const std::size_t nodeCount = std::min<std::size_t>(tile.nodes.size(), 64);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        const NodeView view = evaluateNode(tile.nodes[index], camera, m_refinePixels);
        if (!view.inView)
            continue; // 屏外节点不参与本轮替换：进入视野时再评估
        bool needsSelf = !view.refine || view.externalSlots == 0 || view.hasInlineSlot;
        if (!needsSelf) {
            // Cesium base traversal 的 REPLACE：这一组命中的外部子文件全部就位
            // 才允许替换；任何一个缺席，本组继续顶上（不显示部分子级，避免粗父级
            // 与细子级叠着渲染）。不做按索引跳级显示孙级。
            const std::size_t before = shownOut.size();
            bool covered = true;
            for (std::size_t slotIndex = 0;
                 slotIndex < tile.nodes[index].rangeSlots.size(); ++slotIndex) {
                if ((view.externalSlots &
                     (1ull << std::min<std::size_t>(slotIndex, 63))) == 0)
                    continue;
                const LodSlot& slot = tile.nodes[index].rangeSlots[slotIndex];
                Tile* child = tileAt(slot.childKey);
                if (child == nullptr || !child->resident || !child->renderReady) {
                    covered = false;
                    break;
                }
                const std::size_t childBefore = shownOut.size();
                const bool childCovered = selectShown(*child, camera, depth + 1, shownOut);
                if (!childCovered || shownOut.size() == childBefore) {
                    covered = false;
                    break;
                }
            }
            if (covered && shownOut.size() > before) {
                childShown = true;
                replacedMask |= 1ull << index;
                // 只有上一轮真正可见的节点才构成"看得见的切换"。
                if (tile.shown && ((tile.shownMask >> index) & 1ull) != 0ull) {
                    std::uint32_t deepest = 0;
                    for (std::size_t k = before; k < shownOut.size(); ++k)
                        deepest = std::max(deepest,
                                           static_cast<std::uint32_t>(shownOut[k].tile->level));
                    const int jump = static_cast<int>(deepest) - tile.level;
                    ++m_switchCount;
                    m_switchJumpSum += static_cast<std::uint64_t>(std::max(jump, 0));
                    m_switchJumpMax = std::max(m_switchJumpMax, jump);
                }
            } else {
                ++m_fallbackPasses; // 子树交不出内容：本组退回自身几何
                shownOut.resize(before);
                needsSelf = true;
            }
        }
        if (needsSelf)
            selfMask |= 1ull << index;
    }
    // 嵌套 PagedLOD：外层组已被更细的子瓦片替换时，内层组在本文件里的粗几何
    // 也不该再显示（子瓦片覆盖的是整个外层区域）。
    if (replacedMask != 0) {
        for (std::size_t index = 0; index < nodeCount; ++index) {
            if ((selfMask & (1ull << index)) == 0)
                continue;
            int parent = tile.nodes[index].parentIndex;
            for (int hops = 0; parent >= 0 && hops <= kMaxLodDepth; ++hops) {
                if (parent >= static_cast<int>(nodeCount))
                    break;
                if ((replacedMask >> parent) & 1ull) {
                    selfMask &= ~(1ull << index);
                    break;
                }
                parent = tile.nodes[static_cast<std::size_t>(parent)].parentIndex;
            }
        }
    }
    if (tile.nodes.size() > 64)
        selfMask |= 1ull << 63; // 没有独立位的尾部组：跟随第 63 位
    if (tile.shown && tile.shownMask != 0) {
        const std::uint64_t reverted =
            selfMask & ~tile.shownMask & nodeMaskAll(tile.nodes.size());
        for (std::uint64_t bits = reverted; bits != 0; bits >>= 1)
            m_revertCount += bits & 1ull;
    }
    for (std::uint64_t bits = replacedMask; bits != 0; bits >>= 1)
        m_replacedGroups += bits & 1ull;
    if (m_keepParents && tile.displayable)
        selfMask = nodeMaskAll(tile.nodes.size());
    // 文件自身几何（不属于任何 PagedLOD）不可被替换；有它时图层必须可见。
    const bool selfVisible = tile.displayable &&
        (selfMask != 0 || tile.hasRootGeometry || tile.nodes.empty());
    if (selfVisible)
        shownOut.push_back({&tile, selfMask});
    return selfVisible || childShown;
}

int OsgbStreaming::inFlightCount() const
{
    int count = 0;
    for (const auto& entry : m_tiles) {
        if (entry.second->loading)
            ++count;
    }
    return count;
}

void OsgbStreaming::requestLoads()
{
    if (m_workers.empty()) {
        m_workers.reserve(static_cast<std::size_t>(m_settings.workers));
        for (int index = 0; index < m_settings.workers; ++index)
            m_workers.emplace_back([this] { workerMain(); });
    }
    QVector<Tile*> tiers[kTierCount];
    const qint64 now = nowMs();
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        if (tile->resident || tile->loading)
            continue;
        if (tile->failed && now < tile->retryAtMs)
            continue;
        if (tile->demanded)
            tiers[tile->inCenterCone ? TierCenter : TierView].append(tile);
        else if (tile->prefetch && tile->cooldownUntilMs <= now)
            tiers[TierPrefetch].append(tile); // 刚被淘汰的不再立刻预取回来
    }
    m_pendingDemand =
        static_cast<std::uint32_t>(tiers[TierCenter].size() + tiers[TierView].size());
    const auto byPriority = [](const Tile* left, const Tile* right) {
        // 先按"超出误差阈值多少"排：近处超得多，最该先补；再按屏幕中心角与距离。
        if (left->demandUrgency != right->demandUrgency)
            return left->demandUrgency > right->demandUrgency;
        if (left->demandAngle != right->demandAngle)
            return left->demandAngle < right->demandAngle;
        if (left->demandDistance != right->demandDistance)
            return left->demandDistance < right->demandDistance;
        return left->level > right->level; // 同位置时先补更细的一级
    };
    for (QVector<Tile*>& tier : tiers)
        std::sort(tier.begin(), tier.end(), byPriority);

    // GPU 上传积压：CPU 转换比每帧 8 ms 的上传快得多，先让已转好的内容上屏。
    // 否则积压上百块、最后集中替换父级，画面上就是成片跳变。
    if (m_uploadPending >= m_settings.maxUploadPending)
        return;

    // 在飞上限 + 预算闸门：超预算时继续读只会立刻被淘汰，纯浪费。
    // 上限用 cacheBytes + overflowBytes（Cesium 的 maximumCacheOverflowBytes）：
    // 当前视角需要的内容允许临时超出软上限，只有连余量都装不下时才抬高误差阈值。
    const double averageBytes =
        m_residentTiles > 0 ? double(m_retainedBytes) / double(m_residentTiles) : 1.0e6;
    const std::uint64_t admitBytes = m_settings.cacheBytes + m_settings.overflowBytes;
    m_budgetBlocked = false;
    int inFlight = inFlightCount();
    for (int tier = 0; tier < kTierCount; ++tier) {
        if (tier == TierPrefetch && (m_pendingDemand > 0u || m_hardOverflow))
            break;
        for (Tile* tile : std::as_const(tiers[tier])) {
            if (inFlight >= m_settings.maxInFlight)
                break;
            if (m_residentTiles + static_cast<std::uint32_t>(inFlight) >=
                m_settings.maxTiles) {
                m_budgetBlocked = tier != TierPrefetch;
                break;
            }
            if (m_retainedBytes + static_cast<std::uint64_t>(inFlight) * averageBytes >=
                admitBytes) {
                m_budgetBlocked = tier != TierPrefetch;
                break;
            }
            tile->loading = true;
            ++inFlight;
            Job job;
            job.key = tile->key;
            job.path = tile->path;
            job.angle = tile->demandAngle;
            job.distance = tile->demandDistance;
            job.level = tile->level;
            {
                std::lock_guard<std::mutex> lock(m_jobMutex);
                m_queues[tier].push_back(std::move(job));
            }
            m_jobCondition.notify_one();
        }
    }
}

// 淘汰受害者：先丢"下载完但从未上屏"的更新链尾部，再丢最久没上屏的渲染链尾部。
// 显示中、被需求（子瓦片未就位时的替补）、在飞、宽限期内的都跳过——实测在预算边界
// 上淘汰被需求的替补瓦片会造成无限换入换出，把渲染线程拖到 66 分钟才走完一次冒烟。
void OsgbStreaming::evictToBudget()
{
    m_hardOverflow = m_retainedBytes > m_settings.cacheBytes + m_settings.overflowBytes;
    // 淘汰要留出低水位余量：只退到上限本身，派发端每轮就只能塞进一两块，
    // 表现为"预算明明有空间却卡在 14 块/秒"。留 1/16 容量（约 20 块）当缓冲。
    const double average =
        m_residentTiles > 0 ? double(m_retainedBytes) / double(m_residentTiles) : 3.0e6;
    const std::uint64_t headroom = std::max<std::uint64_t>(
        m_settings.cacheBytes / 16, static_cast<std::uint64_t>(8.0 * average));
    const std::uint64_t softTarget =
        m_settings.cacheBytes > headroom ? m_settings.cacheBytes - headroom
                                         : m_settings.cacheBytes;
    m_prefetchAllowed = m_retainedBytes < softTarget;
    const qint64 now = nowMs();
    int guard = 0;
    while ((m_retainedBytes > softTarget || m_residentTiles > m_settings.maxTiles) &&
           m_residentTiles > m_rootKeys.size() && guard++ < 4096) {
        Tile* victim = nullptr;
        // 1) 本轮没被需求、也不显示的：按 LRU，先丢"下载完但从未上屏"的。
        for (int list = LruUpdated; list >= 0 && !victim; --list) {
            for (Tile* walk = m_lru[list].tail; walk; walk = walk->lruPrev) {
                if (!walk->resident || walk->loading || m_showThisPass.contains(walk->key))
                    continue;
                // 遍历链上的瓦片一律保护。试过放开（只保护"本轮还需要当替补"的，
                // 子树已接管的祖先允许丢内容留索引）：覆盖没有变好，代价是
                // loaded 16091 里 14346 是重下——89% 重复加载，因为接管它的子级
                // 自己就是下一批被淘汰的对象，父级下一轮又要回来。
                // 结论：预算才是约束，放开源不等于放得开。
                if (walk->demanded)
                    continue;
                if (m_rootKeys.contains(walk->key))
                    continue;
                if (list == LruRendered &&
                    now - walk->lastContentMs < m_settings.keepAliveMs)
                    continue; // 宽限期：抑制边界来回抖动
                victim = walk;
                break;
            }
        }
        // 不再淘汰"仍被需求"的瓦片：它下一轮必然又被父级需求，于是刚下载完就被踢、
        // 再重下——实测静止放大时 loaded 以 390 块/秒线性增长、94% 是重复加载，
        // 画面驻留层级却一动不动。装不下需求由收紧误差阈值解决，不靠淘汰。
        if (!victim)
            return;
        m_scene.removeTile(m_groupKey, victim->key);
        lruUnlink(*victim);
        m_retainedBytes =
            m_retainedBytes > victim->bytes ? m_retainedBytes - victim->bytes : 0;
        if (m_residentTiles > 0)
            --m_residentTiles;
        victim->resident = false;
        victim->displayable = false;
        victim->shown = false;
        victim->shownMask = 0;
        victim->renderReady = false;
        victim->bytes = 0;
        // nodes 保留：内容淘汰、索引常驻。父级仍能重新发现并加载它，
        // 但显示上必须等它重新就位才替换父级（REPLACE 不做跳级显示）。
        victim->cooldownUntilMs = now + std::max(500, m_settings.keepAliveMs);
        ++m_evictTotal;
    }
}

// L0 索引层遗忘：骨架自身也有上限，长时间漫游时不能只增不减。
// 被丢弃的瓦片仍可由父级 LodSlot 里的 childFile/childKey 重新发现，不丢可达性。
void OsgbStreaming::forgetIndexes()
{
    if (m_indexBytes <= m_settings.indexCacheBytes)
        return;
    std::vector<Tile*> victims;
    victims.reserve(m_tiles.size());
    const qint64 now = nowMs();
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        if (tile->resident || tile->loading || tile->demanded || tile->prefetch ||
            tile->lruList >= 0 || m_showThisPass.contains(tile->key) ||
            m_rootKeys.contains(tile->key))
            continue;
        if (now - tile->lastIndexMs < m_settings.keepAliveMs)
            continue;
        victims.push_back(tile);
    }
    if (victims.empty())
        return;
    std::sort(victims.begin(), victims.end(), [](const Tile* left, const Tile* right) {
        return left->lastIndexMs < right->lastIndexMs;
    });
    for (Tile* victim : victims) {
        if (m_indexBytes <= m_settings.indexCacheBytes)
            break;
        m_indexBytes =
            m_indexBytes > victim->indexBytes ? m_indexBytes - victim->indexBytes : 0;
        m_tiles.erase(victim->key);
    }
}

void OsgbStreaming::noteFailure(Tile& tile, const QString& reason)
{
    tile.loading = false;
    tile.failed = true;
    ++tile.failureStreak;
    ++m_failedTotal;
    // 指数退避；连续失败判为结构性问题，停止重试并只记一次日志。
    if (tile.failureStreak >= 5) {
        tile.retryAtMs = std::numeric_limits<qint64>::max();
        std::cerr << "VIEW3D_OSBG_TILE_DROPPED key=" << tile.key.toStdString()
                  << " streak=" << tile.failureStreak << " reason=" << reason.toStdString()
                  << std::endl;
        return;
    }
    tile.retryAtMs = nowMs() + (qint64(1000) << std::min(tile.failureStreak, 5));
    std::cerr << "VIEW3D_OSBG_CHILD_FAILED key=" << tile.key.toStdString()
              << " streak=" << tile.failureStreak << " reason=" << reason.toStdString()
              << std::endl;
}

std::size_t OsgbStreaming::estimatedIndexBytes(const ConvertedTile& converted) const
{
    std::size_t bytes = 64;
    for (const TileLodNode& node : converted.lodNodes) {
        bytes += 64;
        for (const TileLodSlot& slot : node.rangeSlots)
            bytes += 96 + slot.childFile.size() * 2;
    }
    return bytes;
}

void OsgbStreaming::applyVisibility()
{
    std::uint32_t shown = 0;
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        if (!tile->resident || !tile->displayable)
            continue;
        const auto found = m_showThisPass.constFind(tile->key);
        const bool wantShown = found != m_showThisPass.constEnd();
        if (!wantShown) {
            if (tile->shown) {
                tile->shown = false;
                tile->shownMask = 0;
                m_scene.setTileVisible(m_groupKey, tile->key, false);
            }
            continue;
        }
        const std::uint64_t mask = found.value();
        const bool wasShown = tile->shown;
        if (!wasShown) {
            tile->shown = true;
            m_scene.setTileVisible(m_groupKey, tile->key, true);
        }
        // 重新可见时必须整份重下发：rootNode->setVisible(cascade) 会覆盖子节点标志。
        if (!wasShown || tile->shownMask != mask) {
            m_scene.setTileNodeMask(m_groupKey, tile->key, mask);
            tile->shownMask = mask;
        }
        ++shown;
        tile->lastContentMs = nowMs(); // 内容真正被用到，才刷新内容宽限期
        lruTouch(*tile);
        m_deepestLevel = std::max(m_deepestLevel, static_cast<std::uint32_t>(tile->level));
    }
    m_shownTiles = shown;
}

void OsgbStreaming::logProgress()
{
    const qint64 now = nowMs();
    if (now - m_lastLogMs < 2000)
        return;
    m_lastLogMs = now;
    // 调度决定的显隐与场景实际状态是否一致：不一致就意味着粗父瓦片和细子瓦片
    // 会同时出现在画面上。分层直方图回答"细分停在哪一层、被什么卡住"。
    int visibilityMismatch = 0;
    int squatting = 0; // 常驻但不显示：预算被不渲染的内容占住的规模
    int nodeMax = 0;   // 单个文件里的 PagedLOD 组数：判断"按文件替换"有多粗
    double nodeSum = 0.0;
    int nodeTiles = 0;
    std::array<int, 16> byResident {};
    std::array<int, 16> byShown {};
    std::array<int, 16> byPending {};
    // 直方图按"实际存在的层级排名"分桶，不是按层级数值：成果起始层各不相同
    // （含山 L12、大雁塔 L15、台北101 L01），按数值夹到 0..15 会把深层数据全挤进
    // 最后一格，分层分布直接失去可读性——上一轮就是靠这条直方图判的收敛层级。
    std::vector<int> levelRanks;
    levelRanks.reserve(32);
    for (const auto& entry : m_tiles) {
        const int level = entry.second->level;
        if (std::find(levelRanks.begin(), levelRanks.end(), level) == levelRanks.end())
            levelRanks.push_back(level);
    }
    std::sort(levelRanks.begin(), levelRanks.end());
    const auto levelBucket = [&levelRanks](int level) {
        const auto rank =
            std::lower_bound(levelRanks.begin(), levelRanks.end(), level) - levelRanks.begin();
        return std::clamp<int>(static_cast<int>(rank), 0, 15);
    };
    for (const auto& entry : m_tiles) {
        const Tile* tile = entry.second.get();
        const int bucket = levelBucket(tile->level);
        if (tile->resident) {
            ++byResident[bucket];
            nodeMax = std::max(nodeMax, static_cast<int>(tile->nodes.size()));
            nodeSum += static_cast<double>(tile->nodes.size());
            ++nodeTiles;
        }
        if (tile->resident && !tile->shown)
            ++squatting;
        if (tile->shown)
            ++byShown[bucket];
        if (tile->demanded && !tile->resident)
            ++byPending[bucket];
        if (tile->resident && tile->displayable &&
            m_scene.isTileVisible(m_groupKey, tile->key) != tile->shown)
            ++visibilityMismatch;
    }
    // 仍在显示"自身几何"的 PagedLOD 组数：被子级替换的组不计。
    // 这个数配合 shown 能看出替换粒度是否真的降到节点（而不是整文件）。
    int selfGroups = 0;
    for (const auto& entry : m_showThisPass) {
        std::uint64_t mask = entry;
        while (mask != 0) {
            selfGroups += static_cast<int>(mask & 1ull);
            mask >>= 1;
        }
    }
    const auto join = [](const std::array<int, 16>& values) {
        QByteArray text;
        for (int index = 0; index < 16; ++index) {
            if (index > 0)
                text.append(',');
            text.append(QByteArray::number(values[index]));
        }
        return text;
    };
    // 桶号到真实层级的对照，缺了它直方图无法判读（第 i 桶 = 第 i 浅的层级）。
    QByteArray rankMap;
    for (std::size_t rank = 0; rank < levelRanks.size() && rank < 16; ++rank) {
        if (rank > 0)
            rankMap.append('/');
        rankMap.append(QByteArray::number(levelRanks[rank]));
    }
    int queues[kTierCount] = {0, 0, 0};
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        for (int tier = 0; tier < kTierCount; ++tier)
            queues[tier] = static_cast<int>(m_queues[tier].size());
    }
    std::cerr << "VIEW3D_OSBG_STREAM resident=" << m_residentTiles
              << " shown=" << m_shownTiles << " shown_lv=" << join(byShown).constData()
              << " visMismatch=" << visibilityMismatch
              << " self_groups=" << selfGroups
              << " replaced=" << m_replacedGroups
              << " switch_n=" << m_switchCount
              << " jump_avg="
              << (m_switchCount > 0
                      ? double(m_switchJumpSum) / double(m_switchCount)
                      : 0.0)
              << " jump_max=" << m_switchJumpMax
              << " revert_n=" << m_revertCount
              << " shared=" << m_sharedChildEdges
              << " deepest=" << m_deepestLevel << " loaded=" << m_loadedTotal
              << " reload=" << m_reloadTotal << " evict=" << m_evictTotal
              << " failed=" << m_failedTotal << " pending=" << m_pendingDemand
              << " inflight=" << inFlightCount()
              << " upload_pending=" << m_uploadPending
              << " sse=" << m_settings.screenSpaceErrorPixels
              << " sse_eff=" << m_refinePixels
              << " dse800="
              << (m_settings.dynamicScreenSpaceError
                      ? (1.0 - std::exp(-std::pow(800.0 * m_dseDensity, 2.0))) *
                          m_settings.dseFactor
                      : 0.0)
              << " blocked=" << (m_budgetBlocked ? 1 : 0)
              << " cam_h=" << m_cameraHeightAboveGround
              << " cam_hc=" << m_cameraHeightAboveCenter
              << " squat=" << squatting
              << (m_hardOverflow ? " OVERFLOW" : "")
              << " since_start_ms=" << (now - m_startMs)
              << " first_shown_ms=" << (m_firstShownMs ? m_firstShownMs - m_startMs : 0)
              << " steady_ms=" << m_steadyMs << std::endl;
    std::cerr << "VIEW3D_OSBG_CACHE mb=" << (m_retainedBytes / (1024.0 * 1024.0)) << "/"
              << (m_settings.cacheBytes >> 20)
              << " index_mb=" << (m_indexBytes / (1024.0 * 1024.0)) << "/"
              << (m_settings.indexCacheBytes >> 20) << " known=" << m_tiles.size()
              << " hi=" << queues[TierCenter] << " med=" << queues[TierView]
              << " lo=" << queues[TierPrefetch] << " useful=" << m_hitTiles
              << " wasted=" << m_missTiles << " fallback=" << m_fallbackPasses
              << " empty=" << m_counts.tilesEmpty << " read_ms=" << m_readMs
              << " convert_ms=" << m_convertMs
              << " node_avg=" << (nodeTiles > 0 ? nodeSum / nodeTiles : 0.0)
              << " node_max=" << nodeMax
              << " lv_map=" << rankMap.constData()
              << " res_lv=" << join(byResident).constData()
              << " pend_lv=" << join(byPending).constData()
              << " tex=" << m_counts.texturesSeen << "/" << m_counts.texturesConverted << "/"
              << m_counts.texturesRejected << " primTex=" << m_counts.texturedPrimitives << "/"
              << m_counts.primitives << " dbl=" << m_counts.doubleSidedPrimitives << "/"
              << m_counts.primitives << " min=" << m_boundsMinimum[0] << ","
              << m_boundsMinimum[1] << "," << m_boundsMinimum[2]
              << " max=" << m_boundsMaximum[0] << "," << m_boundsMaximum[1] << ","
              << m_boundsMaximum[2] << std::endl;
}

// 数据集登记后把相机框到整个范围：包围盒要等预处理算出，所以在前几轮里重试。
// 不框的话相机停在原点上，等于站在模型内部，会立刻请求视野内所有位置的最深层瓦片。
void OsgbStreaming::frameOnce()
{
    if (m_framed)
        return;
    if (!m_layout || !m_layout->primaryView()) {
        m_framed = true;
        return;
    }
    if (nowMs() - m_startMs > 4000) {
        m_framed = true;
        std::cerr << "VIEW3D_OSBG_FRAME_SKIPPED group=" << m_groupKey.toStdString() << std::endl;
        return;
    }
    View3D::IRenderSurface* view = m_layout->primaryView();
    if (!view->focusTileGroup(m_groupKey))
        return;
    m_framed = true;
    std::cerr << "VIEW3D_OSBG_FRAMED group=" << m_groupKey.toStdString()
              << " ms=" << (nowMs() - m_startMs) << std::endl;
}

void OsgbStreaming::tick()
{
    frameOnce();
    // 相机框到数据集之前不做需求排队：默认相机在原点，等于站在模型内部，
    // 那几轮会把周围所有位置的最深层瓦片全部请求下来。
    if (!m_framed)
        return;
    CameraFrame camera;
    if (!pickCamera(&camera))
        return;
    // Cesium 的 DSE 密度：以数据集高度为基准，相机越高系数越小；视线越平
    // （越接近街景）系数越大。逐瓦片误差里减去 1-exp(-(d×density)^2)×factor。
    camera.dseDensity = 0.0;
    if (m_settings.dynamicScreenSpaceError && m_settings.dseFactor > 0.0 && m_hasBounds) {
        const double minimum = m_boundsMinimum[1];
        const double maximum = m_boundsMaximum[1];
        const double span = std::max(1.0e-3, maximum - minimum);
        const double closeHeight =
            minimum + span * std::clamp(m_settings.dseHeightFalloff, 0.01, 1.0);
        const double t = std::clamp(
            (camera.position[1] - closeHeight) / std::max(1.0e-3, maximum - closeHeight),
            0.0, 1.0);
        const double horizon = 1.0 - std::abs(camera.direction[1]);
        camera.dseDensity = std::max(0.0, m_settings.dseDensity * horizon * (1.0 - t));
    }
    m_dseDensity = camera.dseDensity;
    // 两个高度基准都报：离地高度用于 DSE 与日志，球心高度只用于对照。
    m_cameraHeightAboveGround =
        m_hasBounds ? std::max(0.0, camera.position[1] - m_boundsMinimum[1]) : 0.0;
    m_cameraHeightAboveCenter =
        m_hasBounds
            ? std::max(0.0, camera.position[1] -
                                 0.5 * (m_boundsMinimum[1] + m_boundsMaximum[1]))
            : 0.0;
    refreshRenderReadiness();
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        tile->demanded = false;
        tile->prefetch = false;
        tile->inCenterCone = false;
        tile->demandDistance = std::numeric_limits<double>::max();
        tile->demandAngle = std::numeric_limits<double>::max();
        tile->demandUrgency = 0.0;
    }
    m_showThisPass.clear();
    ++m_passSerial;
    m_replacedGroups = 0;
    m_switchCount = 0;
    m_switchJumpSum = 0;
    m_switchJumpMax = 0;
    m_revertCount = 0;
    std::vector<ShownEntry> shown;
    for (const QString& key : std::as_const(m_rootKeys)) {
        Tile* root = tileAt(key);
        if (root == nullptr || !root->resident)
            continue;
        root->demanded = true;
        propagate(*root, camera, 0);
        selectShown(*root, camera, 0, shown);
    }
    // 同一块瓦片可能被多个父节点共享：掩码按位或，图层只登记一次。
    for (const ShownEntry& entry : shown) {
        std::uint64_t& mask = m_showThisPass[entry.tile->key];
        mask |= entry.nodeMask;
    }
    applyVisibility();

    // 先淘汰再派发：反过来时派发看到的是上一轮的余量，每轮最多只能进一块，
    // 实测把加载速率压在 14 块/秒（预算明明已经腾出来了，只是晚了一拍）。
    evictToBudget();
    requestLoads();
    updateRefineThreshold();
    forgetIndexes();
    updateSteady();
    logProgress();
}

// Cesium memoryAdjustedScreenSpaceError：当前需求装不下时把阈值缓慢放大，
// 让整屏一起退到装得下的那一层；低于软上限时再缓慢回落。步长按 25 Hz 的
// 节拍取 4%，约等于 Cesium 60 Hz 的 2%。这样需求只沿阈值缓慢平移，不会像
// 比例控制那样在 16↔128 px 之间整圈翻进翻出（那会造成 88% 重复加载）。
void OsgbStreaming::updateRefineThreshold()
{
    const double base = m_settings.screenSpaceErrorPixels;
    const bool overBudget =
        m_retainedBytes > m_settings.cacheBytes + m_settings.overflowBytes;
    if ((overBudget || m_budgetBlocked) && m_pendingDemand > 0u) {
        m_refinePixels = std::min(m_refinePixels * 1.04, 512.0);
    } else if (m_retainedBytes < m_settings.cacheBytes) {
        m_refinePixels = std::max(base, m_refinePixels / 1.04);
    }
    // 回落条件就按"常驻低于软上限"，不要再放宽。2026-10-29 试过"需求没被挡就回落"
    // （含带淘汰稳定期两种版本），两份数据集的实测结论一致：
    //   含山 阈值确实会被冻在 93.46 px，但根因是 range 口径用错造成的过度需求——
    //   把常驻量顶在软上限之上，让条件永远不成立；口径修正后常驻 1235→452 MB，
    //   阈值自然回落到 16 px、deepest 19→22、reload 11→0，不需要额外回落路径。
    //   台北101 一放宽就变成持续换入换出：resident/shown 一动不动（402/306），
    //   但 reload 0→52、evict 187→237 以约 10 块/秒的速度涨，纯粹是白读盘。
    // 也就是说"冻在粗层"是症状，不是独立缺陷；阈值只能等内存真正退回去再回落。
}

// GPU 上传是异步的：resident 只代表 CPU 内容就位。父级必须等子级真正可显示
// 才能隐藏，否则切换时会先闪出空洞、再整块突然出现，主观上就是"很生硬"。
void OsgbStreaming::refreshRenderReadiness()
{
    m_uploadPending = 0;
    if (m_legacyReady) {
        // 对照用：旧行为把"CPU 已就绪"当成可显示，父级可能先于子级上屏而隐藏。
        for (const auto& entry : m_tiles) {
            if (entry.second->resident)
                entry.second->renderReady = true;
        }
        return;
    }
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        if (!tile->resident || !tile->displayable || tile->renderReady)
            continue;
        const View3D::LayerRenderState state =
            m_scene.tileRenderState(m_groupKey, tile->key);
        if (state == View3D::LayerRenderState::Ready) {
            tile->renderReady = true;
        } else if (state == View3D::LayerRenderState::Failed) {
            // 上传失败：撤掉占位并交给失败重试，父级继续顶上，不会露洞。
            m_scene.removeTile(m_groupKey, tile->key);
            lruUnlink(*tile);
            m_retainedBytes =
                m_retainedBytes > tile->bytes ? m_retainedBytes - tile->bytes : 0;
            if (m_residentTiles > 0)
                --m_residentTiles;
            tile->resident = false;
            tile->displayable = false;
            tile->shown = false;
            tile->shownMask = 0;
            tile->renderReady = false;
            tile->bytes = 0;
            noteFailure(*tile, QStringLiteral("GPU 上传失败"));
        } else {
            ++m_uploadPending;
        }
    }
}

// 全部需求都已就位且没有在飞任务，视为这一次细分收敛。
void OsgbStreaming::updateSteady()
{
    if (m_firstShownMs == 0 && m_shownTiles > 0)
        m_firstShownMs = nowMs();
    if (m_steadyMs < 0.0 && m_pendingDemand == 0u && inFlightCount() == 0)
        m_steadyMs = double(nowMs() - m_startMs);
}

void OsgbStreaming::detachFromScene()
{
    m_tick.stop();
    for (const auto& entry : m_tiles) {
        Tile* tile = entry.second.get();
        if (!tile->resident)
            continue;
        m_scene.removeTile(m_groupKey, tile->key);
        lruUnlink(*tile);
        m_retainedBytes = m_retainedBytes > tile->bytes ? m_retainedBytes - tile->bytes : 0;
        if (m_residentTiles > 0)
            --m_residentTiles;
        tile->resident = false;
        tile->displayable = false;
        tile->shown = false;
        tile->shownMask = 0;
        tile->renderReady = false;
        tile->bytes = 0;
    }
    m_shownTiles = 0;
}

void OsgbStreaming::adoptConverted(const QString& key, ConvertedTile&& tile)
{
    const auto found = m_tiles.find(key);
    if (found == m_tiles.end())
        return; // 索引已被遗忘：父级下一轮会重新登记它
    Tile& target = *found->second;
    if (target.resident)
        return;
    auto mesh = tile.mesh;
    if (!tile.empty && (!mesh || !m_scene.addTile(m_groupKey, target.key, mesh))) {
        noteFailure(target, tile.error.isEmpty() ? QStringLiteral("场景拒绝登记") : tile.error);
        return;
    }
    if (!target.nodes.empty())
        ++m_reloadTotal; // 曾经过载、被淘汰后又回来了
    target.nodes.clear();
    for (const TileLodNode& node : tile.lodNodes) {
        LodNode convertedNode;
        convertedNode.center = node.center;
        convertedNode.radius = node.radius;
        convertedNode.parentIndex = node.parentIndex;
        convertedNode.pixelSizeMode = node.pixelSizeMode;
        for (const TileLodSlot& slot : node.rangeSlots) {
            LodSlot convertedSlot;
            convertedSlot.minimum = slot.minimum;
            convertedSlot.maximum = slot.maximum;
            convertedSlot.external = slot.external;
            if (slot.external && !slot.childFile.isEmpty()) {
                const QString childPath =
                    QFileInfo(target.path).absoluteDir().absoluteFilePath(slot.childFile);
                convertedSlot.childFile = childPath;
                convertedSlot.childKey = tileKeyFor(childPath);
            }
            convertedNode.rangeSlots.push_back(std::move(convertedSlot));
        }
        target.nodes.push_back(std::move(convertedNode));
    }
    const std::size_t addedIndex = estimatedIndexBytes(tile);
    target.indexBytes += addedIndex;
    m_indexBytes += addedIndex;
    if (qEnvironmentVariableIsSet("VIEW3D_OSGB_DUMP_LOD")) {
        static int dumpedTiles = 0;
        if (dumpedTiles < 8) {
            ++dumpedTiles;
            for (std::size_t nodeIndex = 0; nodeIndex < target.nodes.size(); ++nodeIndex) {
                const LodNode& node = target.nodes[nodeIndex];
                std::cerr << "VIEW3D_OSGB_LOD tile=" << target.key.toStdString()
                          << " node=" << nodeIndex << " parent=" << node.parentIndex
                          << " r=" << node.radius
                          << " slotUnit=" << (node.pixelSizeMode ? "px" : "m")
                          << " slots=" << node.rangeSlots.size();
                for (const LodSlot& slot : node.rangeSlots) {
                    std::cerr << " [" << slot.minimum << "," << slot.maximum << ","
                              << (slot.external ? "ext:" : "inline:");
                    std::cerr << slot.childKey.toStdString() << "]";
                }
                std::cerr << std::endl;
            }
            if (tile.mesh) {
                std::cerr << "VIEW3D_OSGB_DTO tile=" << target.key.toStdString()
                          << " nodes=" << tile.mesh->nodes.size()
                          << " prims=" << tile.mesh->primitives.size();
                for (const View3D::ModelNodeDto& dto : tile.mesh->nodes)
                    std::cerr << " {src=" << dto.sourceNodeIndex << ",first="
                              << dto.firstPrimitive << ",n=" << dto.primitiveCount << "}";
                std::cerr << std::endl;
            }
        }
    }
    target.bytes = estimateRetainedBytes(tile);
    target.resident = true;
    target.displayable = !tile.empty;
    target.renderReady = tile.empty; // 非空内容要等场景上传完成（见 tick 的就绪刷新）
    target.hasRootGeometry = tile.hasRootGeometry;
    if (tile.empty)
        ++m_counts.tilesEmpty;
    target.loading = false;
    target.failed = false;
    target.failureStreak = 0;
    // 场景里新登记的瓦片默认就是可见的；初始状态必须与之一致，否则"决定不显示"
    // 会因为和缓存值相同而不下发，粗父瓦片就与子瓦片叠着渲染。
    // 空文件没有可显示内容，applyVisibility 会跳过它，所以标记直接置为不显示。
    target.shown = target.displayable;
    target.shownMask = nodeMaskAll(target.nodes.size());
    target.lastIndexMs = nowMs();
    ++m_residentTiles;
    ++m_loadedTotal;
    m_retainedBytes += target.bytes;
    m_readMs += tile.readMilliseconds;
    m_convertMs += tile.convertMilliseconds;
    m_counts += tile.counts;
    lruPushFront(LruUpdated, target);
    if (tile.hasBounds) {
        if (!m_hasBounds) {
            m_hasBounds = true;
            m_boundsMinimum = tile.boundsMinimum;
            m_boundsMaximum = tile.boundsMaximum;
        } else {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                m_boundsMinimum[axis] = std::min(m_boundsMinimum[axis], tile.boundsMinimum[axis]);
                m_boundsMaximum[axis] = std::max(m_boundsMaximum[axis], tile.boundsMaximum[axis]);
            }
        }
    }
    m_deepestLevel = std::max(m_deepestLevel, static_cast<std::uint32_t>(target.level));
}

void OsgbStreaming::workerMain()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCondition.wait(lock, [this] {
                return m_stopping || !m_queues[TierCenter].empty() ||
                    !m_queues[TierView].empty() || !m_queues[TierPrefetch].empty();
            });
            if (m_stopping)
                return;
            // 高优先级队列先出，工作线程每次只取一条。
            for (std::deque<Job>& queue : m_queues) {
                if (queue.empty())
                    continue;
                job = queue.front();
                queue.pop_front();
                break;
            }
        }
        auto converted = std::make_shared<ConvertedTile>();
        const bool ok = convertTileFile(job.path, m_origin, converted.get()) &&
            (!m_globeMode || bakeTileToGlobe(*converted));
        QMetaObject::invokeMethod(
            &m_context,
            [this, alive = m_alive, job, converted, ok] {
                if (!alive->load(std::memory_order_acquire))
                    return;
                Tile* tile = tileAt(job.key);
                if (tile != nullptr) {
                    tile->loading = false;
                    if (!ok)
                        noteFailure(*tile, converted->error);
                    else if (!tile->demanded && !tile->prefetch)
                        ++m_missTiles; // 下载完已不需要：一次浪费的读取
                    else
                        ++m_hitTiles;
                }
                if (ok && tile != nullptr)
                    adoptConverted(job.key, std::move(*converted));
            },
            Qt::QueuedConnection);
    }
}

} // namespace View3DOsb
