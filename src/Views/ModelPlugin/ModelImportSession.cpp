#include "ModelImportSession.h"

#include "GeoReproject.h"
#include "ModelReadTypes.h"

#include <GlobeMath.h>
#include <IAppLog.h>
#include <IAppLogManager.h>
#include <IApplication.h>
#include <IPluginContainer.h>
#include <IGeoReference.h>
#include <IRenderScene.h>
#include <ISceneLayerService.h>
#include <IViewportLayout.h>
#include <View3DObjectIds.h>

#include <QDir>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <utility>

namespace View3DModel {
namespace {

QString categorySuffix(ImportFormatKind kind)
{
    switch (kind) {
    case ImportFormatKind::PointCloud:
        return QStringLiteral("点云");
    case ImportFormatKind::Raster:
        return QStringLiteral("栅格");
    case ImportFormatKind::Vector:
        return QStringLiteral("矢量");
    default:
        return QStringLiteral("模型");
    }
}

QString kindText(ImportFormatKind kind)
{
    switch (kind) {
    case ImportFormatKind::PointCloud:
        return QStringLiteral("pointcloud");
    case ImportFormatKind::Raster:
        return QStringLiteral("raster");
    case ImportFormatKind::Vector:
        return QStringLiteral("vector");
    case ImportFormatKind::Tileset:
        return QStringLiteral("tileset");
    case ImportFormatKind::OsgbTiles:
        return QStringLiteral("osgb");
    case ImportFormatKind::Unknown:
        return QStringLiteral("unknown");
    case ImportFormatKind::Model:
    default:
        return QStringLiteral("model");
    }
}

} // namespace

ModelImportSession::ModelImportSession(RF::PluginEngine::IApplication* application)
    : m_application(application)
{
}

ModelImportSession::~ModelImportSession()
{
    stop();
}

bool ModelImportSession::start()
{
    if (m_running)
        return true;
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    if (!container || !container->objectAs<View3D::ISceneLayerService>(
                          View3D::ServiceIds::SceneLayers))
        return false;
    m_cancel = false;
    m_running = true;
    m_worker = std::thread([this] { workerMain(); });
    m_pollTimer = std::make_unique<QTimer>();
    m_pollTimer->setInterval(40);
    QObject::connect(m_pollTimer.get(), &QTimer::timeout, [this] { poll(); });
    m_pollTimer->start();
    return true;
}

void ModelImportSession::stop()
{
    if (!m_running)
        return;
    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancel = true;
        m_queue.clear();
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    m_pollTimer.reset();
    if (m_progress) {
        m_progress->hide();
        delete m_progress;
        m_progress = nullptr;
    }
    if (m_failureBox) {
        m_failureBox->hide();
        delete m_failureBox;
        m_failureBox = nullptr;
    }
    m_batchTotal = 0;
    m_batchDone = 0;
    m_batchFailures.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outcomes.clear();
}

bool ModelImportSession::enqueue(ModelImportJob job, QString* error)
{
    if (job.renderKey.isEmpty()) {
        const IModelFormatReader* reader = modelFormatReaders().findReaderById(job.readerId);
        if (!reader) {
            if (error)
                *error = QStringLiteral("没有已登记的读取器：%1").arg(job.readerId);
            return false;
        }
        job.kind = reader->kind();
        if (!QFileInfo::exists(job.path)) {
            if (error)
                *error = QStringLiteral("文件不存在：%1").arg(job.path);
            return false;
        }
    } else if (!job.path.isEmpty() && !QFileInfo::exists(job.path)) {
        // 重放作业带路径就意味着要重读源文件，文件已经不在了就没法替换。
        if (error)
            *error = QStringLiteral("文件不存在：%1").arg(job.path);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(job));
    }
    m_wake.notify_one();
    // 排队就立刻给反馈：数据量大时读盘+转换要几十秒到几分钟，没有进度就形同卡死。
    if (!m_progress || !m_progress->isVisible())
        beginBatch();
    ++m_batchTotal;
    refreshProgress();
    return true;
}

bool ModelImportSession::submitWithReader(const QString& path, const QString& readerId,
                                          const ModelImportOptions& options,
                                          const View3D::LayerId& parentGroup, QString* error)
{
    ModelImportJob job;
    job.path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    job.readerId = readerId;
    job.options = options;
    if (!job.options.geoReference && m_application && m_application->getPluginContainer()) {
        // 参照系服务由 plugin.view3d.crs 提供；缺失时读取器保留源坐标并在 warnings 里说明。
        job.options.geoReference = m_application->getPluginContainer()->objectAs<
            View3D::IGeoReference>(View3D::ServiceIds::GeoReference);
    }
    job.parentGroup = parentGroup;
    if (!enqueue(std::move(job), error))
        return false;
    std::cerr << "VIEW3D_MODEL_SUBMIT path=" << path.toStdString() << " reader="
              << readerId.toStdString() << std::endl;
    return true;
}


void ModelImportSession::workerMain()
{
    for (;;) {
        ModelImportJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_cancel || !m_queue.empty(); });
            if (m_cancel)
                return;
            job = m_queue.front();
            m_queue.pop_front();
            m_active = 1;
            m_activePath = job.path.isEmpty() ? job.renderKey : job.path;
        }

        ModelImportOutcome outcome;
        outcome.path = job.path;
        outcome.readerId = job.readerId;
        outcome.kind = job.kind;
        outcome.parentGroup = job.parentGroup;
        outcome.options = job.options;
        outcome.renderKey = job.renderKey;
        outcome.replaceLayer = job.replaceLayer;
        outcome.globeBake = job.globeBake;
        const auto started = std::chrono::steady_clock::now();
        if (job.globeBake && !job.renderKey.isEmpty()) {
            // 球面重烘焙：用当前网格重建 ECEF 几何，不读源文件。
            outcome.result = buildGlobeMesh(job);
        } else if (!job.renderKey.isEmpty() && job.path.isEmpty()) {
            // 缓存重放：源地理坐标就在网格上，不必回头读源文件。
            outcome.result = replayCachedMesh(job);
        } else try {
            const IModelFormatReader* reader = modelFormatReaders().findReaderById(job.readerId);
            outcome.result = reader ? reader->read(job.path, job.options) : ModelReadResult {};
            if (!reader) {
                outcome.result.error.code = QStringLiteral("MODEL_READER_MISSING");
                outcome.result.error.message = QStringLiteral("读取器已注销");
            }
        } catch (const std::exception& exception) {
            outcome.result.success = false;
            outcome.result.error.code = QStringLiteral("MODEL_IMPORT_EXCEPTION");
            outcome.result.error.message = QString::fromUtf8(exception.what());
        } catch (...) {
            outcome.result.success = false;
            outcome.result.error.code = QStringLiteral("MODEL_IMPORT_EXCEPTION");
            outcome.result.error.message = QStringLiteral("读取器抛出未知异常");
        }
        if (outcome.result.valid() && outcome.renderKey.isEmpty() && job.options.geoReference &&
            job.options.geoReference->dynamicProjectionEnabled() &&
            !outcome.result.sourceCrs.isEmpty() && outcome.result.mesh->geoVertices.empty()) {
            // 动态投影开着才缓存：逐点反算的开销与每顶点 24 B 都不该花在没启用的场景上。
            QString cacheError;
            if (!cacheGeographicVertices(job.options.geoReference, outcome.result.mesh.get(),
                                         &cacheError))
                outcome.result.warnings.append(cacheError);
        }
        const qint64 elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        outcome.result.format = outcome.result.format.isEmpty()
            ? QFileInfo(job.path).suffix().toLower()
            : outcome.result.format;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancel)
            return;
        outcome.result.statistics.insert(QStringLiteral("elapsedMilliseconds"),
                                         static_cast<qulonglong>(elapsed));
        m_outcomes.push_back(std::move(outcome));
        m_active = 0;
        m_activePath.clear();
    }
}

ModelReadResult ModelImportSession::replayCachedMesh(const ModelImportJob& job) const
{
    ModelReadResult result;
    result.format = QStringLiteral("replay");
    result.readerId = job.readerId;
    const auto fail = [&result](const QString& message) {
        result.success = false;
        result.error.code = QStringLiteral("MODEL_REPLAY_FAILED");
        result.error.message = message;
        return result;
    };
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    if (!scene)
        return fail(QStringLiteral("渲染场景服务未就绪"));
    if (!job.options.geoReference)
        return fail(QStringLiteral("参照系服务未加载"));
    const std::shared_ptr<const View3D::ModelMeshDto> current =
        scene->modelLayerMesh(job.renderKey);
    if (!current)
        return fail(QStringLiteral("图层已不在场景中"));
    if (current->geoVertices.empty() ||
        current->geoVertices.size() != current->vertices.size() * 3)
        return fail(QStringLiteral("图层没有源地理坐标缓存"));
    // 网格交给渲染端后就是只读的共享数据，重放只能另出一份新网格再换上去。
    auto replayed = std::make_shared<View3D::ModelMeshDto>(*current);
    QString error;
    if (!replayGeographicVertices(job.options.geoReference, replayed.get(), &error))
        return fail(error);
    result.success = true;
    result.mesh = std::move(replayed);
    return result;
}

bool ModelImportSession::submitGlobeRebuild(bool enabled, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto* scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto* reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    if (!layers || !scene)
        return fail(QStringLiteral("图层或渲染场景服务未就绪"));
    if (!enabled) {
        // 退出球面：无源地理坐标缓存的图层直接换回烘焙前保留的平面原网格；
        // 有缓存的交给投影重放按当前显示参照系重算。
        int restored = 0;
        for (auto it = m_flatFallback.cbegin(); it != m_flatFallback.cend(); ++it) {
            const View3D::LayerId layer = layers->layerByRenderKey(it.key());
            if (layer.isNull() || !it.value())
                continue;
            if (layers->replaceModelLayerGeometry(layer, it.value()))
                ++restored;
        }
        m_flatFallback.clear();
        QString replayError;
        const bool replayed =
            reference && submitReprojection(reference->displayCrs(), &replayError);
        std::cerr << "VIEW3D_MODEL_GLOBE mode=0 restored=" << restored
                  << " replayed=" << (replayed ? 1 : 0) << std::endl;
        return restored > 0 || replayed;
    }
    int queued = 0;
    for (auto it = m_layerSources.cbegin(); it != m_layerSources.cend(); ++it) {
        // 只处理本会话导入的模型图层：影像/矢量等平面覆盖层的 key 不在这里。
        const View3D::LayerId layer = layers->layerByRenderKey(it.key());
        if (layer.isNull())
            continue;
        if (enqueueGlobeBake(it.key(), layer))
            ++queued;
    }
    std::cerr << "VIEW3D_MODEL_GLOBE mode=1 queued=" << queued << std::endl;
    if (queued == 0)
        return fail(QStringLiteral("没有可球面显示的模型图层"));
    return true;
}

bool ModelImportSession::enqueueGlobeBake(const QString& renderKey,
                                          const View3D::LayerId& layer)
{
    ModelImportJob job;
    job.renderKey = renderKey;
    job.replaceLayer = layer;
    job.globeBake = true;
    return enqueue(std::move(job), nullptr);
}

ModelReadResult ModelImportSession::buildGlobeMesh(const ModelImportJob& job) const
{
    using namespace View3D::GlobeMath;
    ModelReadResult result;
    result.format = QStringLiteral("globe");
    result.readerId = job.readerId;
    const auto fail = [&result](const QString& message) {
        result.success = false;
        result.error.code = QStringLiteral("MODEL_GLOBE_BAKE_FAILED");
        result.error.message = message;
        return result;
    };
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    if (!scene)
        return fail(QStringLiteral("渲染场景服务未就绪"));
    const std::shared_ptr<const View3D::ModelMeshDto> current =
        scene->modelLayerMesh(job.renderKey);
    if (!current || current->vertices.empty())
        return fail(QStringLiteral("图层已不在场景中"));
    if (current->hasContentOrigin) {
        // 已经是球面几何（重复作业）：原样返回，避免把相对偏移再当成本地坐标烘焙。
        result.success = true;
        result.mesh = std::make_shared<View3D::ModelMeshDto>(*current);
        return result;
    }
    // 顶点带节点变换时不在世界位置上，按顶点烘焙会得到错误位置。
    static const std::array<double, 16> kIdentity { 1.0, 0.0, 0.0, 0.0,
                                                    0.0, 1.0, 0.0, 0.0,
                                                    0.0, 0.0, 1.0, 0.0,
                                                    0.0, 0.0, 0.0, 1.0 };
    for (const View3D::ModelNodeDto& node : current->nodes) {
        if (node.localTransform != kIdentity)
            return fail(QStringLiteral("带节点变换的网格暂不支持球面显示"));
    }
    auto mesh = std::make_shared<View3D::ModelMeshDto>(*current);
    const std::size_t count = mesh->vertices.size();
    const bool hasGeo = !mesh->geoCrs.isEmpty() &&
        mesh->geoVertices.size() == count * 3;
    std::vector<Vec3> ecef(count);
    if (hasGeo) {
        // 有源地理坐标：逐顶点 经纬度 → ECEF（与显示参照系无关，直接精确）。
        for (std::size_t i = 0; i < count; ++i) {
            const double lon = mesh->geoVertices[i * 3];
            const double lat = mesh->geoVertices[i * 3 + 1];
            const double height = mesh->geoVertices[i * 3 + 2];
            if (!std::isfinite(lon) || !std::isfinite(lat) || !std::isfinite(height) ||
                std::abs(lon) > 180.0 || std::abs(lat) > 90.0) {
                return fail(QStringLiteral("源地理坐标缓存超出经纬度范围"));
            }
            ecef[i] = geodeticToEcef(lon, lat, height);
        }
    } else {
        // 没有坐标系：局部原点钉在 (0°,0°) 地表，局部 (E,U,−N) 按该点 ENU 摆放。
        const Vec3 anchor = geodeticToEcef(0.0, 0.0, 0.0);
        for (std::size_t i = 0; i < count; ++i) {
            const View3D::MeshVertexDto& vertex = mesh->vertices[i];
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
    for (std::size_t i = 0; i < count; ++i) {
        View3D::MeshVertexDto& vertex = mesh->vertices[i];
        const Vec3 offset = sub(ecef[i], center);
        vertex.x = offset[0];
        vertex.y = offset[1];
        vertex.z = offset[2];
        if (!vertex.hasNormal)
            continue;
        Vec3 east { 0.0, 1.0, 0.0 };
        Vec3 north { 0.0, 0.0, 1.0 };
        Vec3 up { 1.0, 0.0, 0.0 };
        if (hasGeo) {
            const double lonRad = mesh->geoVertices[i * 3] * kRadiansPerDegree;
            const double latRad = mesh->geoVertices[i * 3 + 1] * kRadiansPerDegree;
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
    mesh->hasContentOrigin = true;
    mesh->contentOrigin = center;
    // 顶点已换系，旧的拾取索引不能再用，按新顶点重建。
    mesh->rebuildRayIndex();
    result.success = true;
    result.mesh = std::move(mesh);
    return result;
}

bool ModelImportSession::submitReprojection(const QString& displayCrs, QString* error)
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto* scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto* reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!layers || !scene || !reference)
        return fail(QStringLiteral("参照系或图层服务未就绪，无法重放投影"));

    int queued = 0;
    int skipped = 0;
    QStringList skippedNames;
    for (const QString& key : scene->modelLayerKeys()) {
        // 瓦片由流式插件自己按组变换跟随（倾斜摄影、3D Tiles），这里只管整块图层。
        if (key.startsWith(QLatin1String("tile:")))
            continue;
        const View3D::LayerId layer = layers->layerByRenderKey(key);
        if (layer.isNull())
            continue;
        const std::shared_ptr<const View3D::ModelMeshDto> mesh = scene->modelLayerMesh(key);
        if (!mesh)
            continue;
        ModelImportJob job;
        job.renderKey = key;
        job.replaceLayer = layer;
        job.kind = ImportFormatKind::Model;
        job.options.geoReference = reference;
        const bool cached = !mesh->geoVertices.empty() &&
            mesh->geoVertices.size() == mesh->vertices.size() * 3;
        if (!cached) {
            // 没有源参照系的模型/点云与投影无关，别为了切系去重读它们的文件。
            if (layers->layerSourceCrs(layer).trimmed().isEmpty())
                continue;
            // 超出缓存上限的图层退化成「重读源文件」：结果一样，只是慢。
            const auto source = m_layerSources.constFind(key);
            if (source == m_layerSources.constEnd() || !QFileInfo::exists(source->path)) {
                ++skipped;
                skippedNames.append(layers->layerName(layer));
                continue;
            }
            job.path = source->path;
            job.readerId = source->readerId;
            job.options = source->options;
            job.options.geoReference = reference;
        }
        QString enqueueError;
        if (enqueue(std::move(job), &enqueueError)) {
            ++queued;
        } else {
            ++skipped;
            skippedNames.append(QStringLiteral("%1（%2）").arg(layers->layerName(layer),
                                                               enqueueError));
        }
    }
    if (queued > 0)
        m_batchReplay = true;   // beginBatch 在第一次 enqueue 里已经跑过，标签随后改
    std::cerr << "VIEW3D_MODEL_REPLAY display=" << displayCrs.toStdString() << " queued=" << queued
              << " skipped=" << skipped << std::endl;
    if (!skippedNames.isEmpty()) {
        log(QStringLiteral("%1 个图层未按 %2 重放：%3")
                .arg(skipped)
                .arg(displayCrs, skippedNames.join(QStringLiteral("、"))));
    }
    if (queued == 0)
        return fail(QStringLiteral("没有可按 %1 重放的图层").arg(displayCrs));
    return true;
}

void ModelImportSession::poll()
{
    std::vector<ModelImportOutcome> ready;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_outcomes.empty())
            ready.swap(m_outcomes);
    }
    for (const ModelImportOutcome& outcome : ready) {
        commit(outcome);
        ++m_batchDone;
    }
    if (!m_progress || !m_progress->isVisible())
        return;
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        idle = m_queue.empty() && m_active == 0 && m_outcomes.empty();
    }
    if (idle)
        endBatch();
    else
        refreshProgress();
}

QWidget* ModelImportSession::dialogParent() const
{
    return m_application ? m_application->getMainWindow() : nullptr;
}

void ModelImportSession::beginBatch()
{
    m_batchTotal = 0;
    m_batchDone = 0;
    m_batchCancelled = false;
    m_batchLabel.clear();
    m_batchReplay = false;
    m_batchClock.restart();
    if (!m_progress) {
        m_progress = new QProgressDialog(QString(), QStringLiteral("取消排队"), 0, 1,
                                         dialogParent());
        m_progress->setWindowTitle(QStringLiteral("正在打开数据"));
        m_progress->setWindowModality(Qt::NonModal);
        // 默认要等 4 秒才显示，正好是"选了文件之后没反应"的那段；这里必须归零。
        m_progress->setMinimumDuration(0);
        m_progress->setAutoClose(false);
        m_progress->setAutoReset(false);
        m_progress->setMinimumWidth(420);
        m_progress->setValue(0);
        QObject::connect(m_progress, &QProgressDialog::canceled,
                         [this] { cancelQueued(); });
    }
    m_progress->show();
    m_progress->raise();
}

void ModelImportSession::refreshProgress()
{
    if (!m_progress)
        return;
    QString current;
    int pending = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_activePath.isEmpty())
            current = QFileInfo(m_activePath).fileName();
        else if (!m_queue.empty())
            current = QFileInfo(m_queue.front().path).fileName();
        pending = static_cast<int>(m_queue.size()) + m_active;
    }
    const int total = std::max(1, m_batchTotal);
    const qint64 seconds = m_batchClock.isValid() ? m_batchClock.elapsed() / 1000 : 0;
    QString label;
    // 同一套排队/进度既服务导入也服务投影重放，文案要分清在忙哪件事。
    const QString activity = m_batchReplay ? QStringLiteral("正在按新投影重放")
                                           : QStringLiteral("正在打开数据");
    if (m_batchCancelled)
        label = QStringLiteral("已取消排队：正在处理的这一项仍会完成（已完成 %1/%2）")
                    .arg(m_batchDone)
                    .arg(total);
    else
        label = QStringLiteral("%1（%2/%3）\n当前：%4　已用 %5 秒")
                    .arg(activity)
                    .arg(m_batchDone)
                    .arg(total)
                    .arg(current.isEmpty() ? QStringLiteral("准备中…") : current)
                    .arg(seconds);
    if (label != m_batchLabel) {
        m_batchLabel = label;
        m_progress->setLabelText(label);
    }
    m_progress->setRange(0, total);
    m_progress->setValue(std::min(m_batchDone, total));
}

void ModelImportSession::endBatch()
{
    if (m_progress)
        m_progress->hide();
    m_batchTotal = 0;
    m_batchDone = 0;
    m_batchCancelled = false;
    m_batchLabel.clear();
    m_batchReplay = false;
    showFailures();
}

void ModelImportSession::cancelQueued()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }
    m_batchCancelled = true;
    if (m_progress)
        m_progress->setCancelButton(nullptr);
    std::cerr << "VIEW3D_MODEL_CANCEL_QUEUED" << std::endl;
    refreshProgress();
}

void ModelImportSession::showFailures()
{
    if (m_batchFailures.isEmpty())
        return;
    constexpr int kShownLines = 8;
    QString detail;
    const int shown = std::min(kShownLines, static_cast<int>(m_batchFailures.size()));
    for (int index = 0; index < shown; ++index) {
        if (index > 0)
            detail += QLatin1Char('\n');
        detail += m_batchFailures.at(index);
    }
    if (m_batchFailures.size() > shown) {
        detail += QStringLiteral("\n…还有 %1 个未列出").arg(m_batchFailures.size() - shown);
    }
    const int failed = m_batchFailures.size();
    m_batchFailures.clear();

    // 非模态：这里正处在轮询定时器的回调里，模态 exec() 会再开一层事件循环，
    // 把驱动它的调用者压在栈底（无头冒烟实测会直接卡住），也让用户在导入
    // 还在跑的时候动不了界面。窗口复用，后续失败只更新文案并置顶。
    if (!m_failureBox) {
        m_failureBox = new QMessageBox(dialogParent());
        m_failureBox->setIcon(QMessageBox::Warning);
        m_failureBox->setWindowTitle(QStringLiteral("打开数据失败"));
        m_failureBox->setStandardButtons(QMessageBox::Ok);
        m_failureBox->setModal(false);
    }
    m_failureBox->setText(QStringLiteral("%1 个文件没能打开：").arg(failed));
    m_failureBox->setInformativeText(detail);
    m_failureBox->show();
    m_failureBox->raise();
    std::cerr << "VIEW3D_MODEL_FAILURE_DIALOG files=" << failed << std::endl;
}

void ModelImportSession::commit(const ModelImportOutcome& outcome)
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto* scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    const QString fileName = QFileInfo(outcome.path).fileName();
    // 重放作业没有文件路径，失败提示要说得出是哪个图层。
    const QString subject = outcome.renderKey.isEmpty() && !fileName.isEmpty()
        ? fileName
        : (layers && !outcome.replaceLayer.isNull() ? layers->layerName(outcome.replaceLayer)
                                                   : QStringLiteral("图层"));

    if (!outcome.result.valid() || !layers) {
        const QString reason = outcome.result.error.isEmpty()
            ? QStringLiteral("文件内没有可显示的几何")
            : outcome.result.error.text();
        m_batchFailures.append(QStringLiteral("%1：%2").arg(subject, reason));
        std::cerr << "VIEW3D_MODEL_FAILED path=" << subject.toStdString() << " reason="
                  << reason.toStdString() << std::endl;
        log(QStringLiteral("导入失败 %1：%2").arg(subject, reason));
        return;
    }

    if (!outcome.renderKey.isEmpty()) {
        // 模式已变的过期结果直接丢弃：例如切回平面后，球面烘焙作业才完成。
        if (scene && outcome.globeBake != scene->globeModeEnabled()) {
            std::cerr << "VIEW3D_MODEL_" << (outcome.globeBake ? "GLOBE" : "REPLAY")
                      << "_DROPPED key=" << outcome.renderKey.toStdString() << std::endl;
            return;
        }
        // 换几何，不新建图层；失败就保留上屏的旧网格，只报一条。
        if (outcome.globeBake && scene) {
            // 无源地理坐标缓存的图层：留住烘焙前的平面原网格，退出球面直接换回。
            const auto current = scene->modelLayerMesh(outcome.renderKey);
            if (current && !current->hasContentOrigin &&
                (current->geoCrs.isEmpty() ||
                 current->geoVertices.size() != current->vertices.size() * 3))
                m_flatFallback.insert(outcome.renderKey, current);
        }
        if (layers->replaceModelLayerGeometry(outcome.replaceLayer, outcome.result.mesh)) {
            std::cerr << "VIEW3D_MODEL_"
                      << (outcome.globeBake ? "GLOBE_READY" : "REPLAYED")
                      << " key=" << outcome.renderKey.toStdString()
                      << " vertices=" << outcome.result.mesh->vertices.size() << std::endl;
            const QString summary = outcome.globeBake
                ? QStringLiteral("已按球面重烘焙 %1（%2 个顶点）")
                : QStringLiteral("已按新投影重放 %1（%2 个顶点）");
            log(summary.arg(subject).arg(outcome.result.mesh->vertices.size()));
        } else {
            m_batchFailures.append(QStringLiteral("%1：几何替换失败").arg(subject));
            std::cerr << "VIEW3D_MODEL_"
                      << (outcome.globeBake ? "GLOBE_FAILED" : "REPLAY_FAILED")
                      << " key=" << outcome.renderKey.toStdString() << std::endl;
        }
        return;
    }

    // 渲染键带序号：同名文件从不同目录重复导入时不能互相顶掉。
    const QString renderKey = QStringLiteral("model:%1:%2")
                                  .arg(fileName)
                                  .arg(++m_layerSequence, 4, 10, QLatin1Char('0'));
    const QString name = QStringLiteral("%1（%2）")
                             .arg(QFileInfo(outcome.path).completeBaseName(),
                                  categorySuffix(outcome.kind));
    // 参照系展示文本：EPSG 码后补 PROJ 官方名称，属性面板直接可读。
    QString displayCrs = outcome.result.sourceCrs;
    auto* reference = container->objectAs<View3D::IGeoReference>(
        View3D::ServiceIds::GeoReference);
    if (reference && !displayCrs.isEmpty()) {
        View3D::IGeoReference::CrsInfo info;
        if (reference->crsInfo(displayCrs, &info) && !info.name.isEmpty())
            displayCrs = QStringLiteral("%1（%2）").arg(displayCrs, info.name);
    }
    const View3D::LayerId layer =
        layers->addModelLayer(name, renderKey, outcome.result.mesh, outcome.parentGroup,
                             displayCrs);
    if (layer.isNull()) {
        m_batchFailures.append(QStringLiteral("%1：图层登记失败").arg(fileName));
        std::cerr << "VIEW3D_MODEL_FAILED path=" << fileName.toStdString()
                  << " reason=layer-register" << std::endl;
        return;
    }
    // 记住源文件与选项：源坐标缓存超上限的图层，切换投影时只能按同样的参数重读。
    m_layerSources.insert(renderKey, LayerSource {outcome.path, outcome.readerId, outcome.options});
    // 球面模式下新导入的图层异步重烘焙上球面（先按平面几何登记，再换 ECEF）。
    if (scene && scene->globeModeEnabled())
        enqueueGlobeBake(renderKey, layer);
    // 参照系框架：矢量/栅格重投影过才填。源→局部要经过椭球投影与 ENU 切平面，
    // 不是仿射变换，所以只给世界原点经纬度，不给 4×4 矩阵。
    if (!outcome.result.sourceCrs.isEmpty() &&
        outcome.result.statistics.value(QStringLiteral("reprojectedToMainFrame")).toBool()) {
        View3D::ILayer::ReferenceFrame frame;
        if (reference) {
            double lon = 0.0;
            double lat = 0.0;
            double height = 0.0;
            if (reference->origin(&lon, &lat, &height)) {
                frame.position = QStringLiteral("%1°, %2°, 椭球高 %3 m")
                                     .arg(lon, 0, 'f', 8)
                                     .arg(lat, 0, 'f', 8)
                                     .arg(height, 0, 'f', 3);
            }
        }
        layers->setLayerReferenceFrame(layer, frame);
    }

    for (const QString& warning : outcome.result.warnings) {
        std::cerr << "VIEW3D_MODEL_WARNING path=" << fileName.toStdString() << " warning="
                  << warning.toStdString() << std::endl;
        log(QStringLiteral("%1：%2").arg(fileName, warning));
    }
    QStringList statistics;
    for (auto it = outcome.result.statistics.constBegin();
         it != outcome.result.statistics.constEnd(); ++it) {
        if (it.value().typeId() == QMetaType::QString &&
            it.value().toString().size() > 120) {
            // WKT 坐标系的整段文本对日志没有意义，统计里留前缀即可。
            statistics.append(QStringLiteral("%1=%2…")
                                  .arg(it.key(), it.value().toString().left(60)));
            continue;
        }
        statistics.append(QStringLiteral("%1=%2").arg(it.key(), it.value().toString()));
    }
    std::cerr << "VIEW3D_MODEL_IMPORTED path=" << fileName.toStdString() << " reader="
              << outcome.result.readerId.toStdString() << " kind="
              << kindText(outcome.kind).toStdString() << " vertices="
              << outcome.result.mesh->vertices.size() << " primitives="
              << outcome.result.mesh->primitives.size() << " textures="
              << outcome.result.mesh->textures.size() << " ms="
              << outcome.result.statistics.value(QStringLiteral("elapsedMilliseconds")).toString().toStdString()
              << std::endl;
    log(QStringLiteral("已导入 %1（%2）").arg(fileName, statistics.join(QStringLiteral(" "))));

    // 新数据落图层后把每个视口定到它上面，否则大坐标数据会“导入成功但看不见”。
    // 视口的定位接口按渲染键取，不是按图层 id。
    if (auto* layout = container->objectAs<View3D::IViewportLayout>(
            View3D::ServiceIds::ViewportLayout)) {
        const QList<View3D::IRenderSurface*> views = layout->views();
        for (View3D::IRenderSurface* view : views) {
            if (view)
                view->focusModelLayer(renderKey);
        }
        if (views.isEmpty() && layout->activeView())
            layout->activeView()->focusModelLayer(renderKey);
    }
}

void ModelImportSession::log(const QString& message) const
{
    auto* manager = m_application ? m_application->getAppLogManager() : nullptr;
    if (auto* logger = manager ? manager->getLogger(QStringLiteral("model-import")) : nullptr)
        logger->info(message);
}

} // namespace View3DModel
