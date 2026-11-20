#include "OgreScene.h"

#include "OgreRuntime.h"

#include <GlobeMath.h>
#include <ModelMesh.h>

#include <OgreAxisAlignedBox.h>
#include <OgreCamera.h>
#include <OgreColourValue.h>
#include <OgreException.h>
#include <OgreHlms.h>
#include <OgreHlmsDatablock.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreIdString.h>
#include <OgrePixelFormatGpu.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreLight.h>
#include <OgreManualObject2.h>
#include <OgreMatrix4.h>
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

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <mutex>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <tuple>

namespace View3D {

namespace {

constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

// 把一块 datablock 会被设置的全部字段压成签名：签名相同就意味着两块完全一样，
// 可以共用同一块。以前每个基元各建一块，98,845 基元的 3DS 厂区模型在销毁第
// ~549 块时直接 fastfail（0xc0000409），而且每帧绑定与内存都被这个数量拖大。
std::string materialSignature(const ModelPrimitiveDto& primitive,
                              const std::vector<ModelTextureDto>& textures,
                              bool unlitChannel)
{
    auto append = [](std::string& out, const void* value, std::size_t size) {
        out.append(static_cast<const char*>(value), size);
    };
    auto appendIndex = [&append, &textures](std::string& out, std::int32_t index,
                                            std::int32_t channel) {
        append(out, &index, sizeof(index));
        append(out, &channel, sizeof(channel));
        // 采样器与尺寸也是外观的一部分：同贴图不同 filter/wrap 不能共用一块。
        const std::int32_t key[5] = {
            index >= 0 && static_cast<std::size_t>(index) < textures.size()
                ? textures[std::size_t(index)].magFilter : 0,
            index >= 0 && static_cast<std::size_t>(index) < textures.size()
                ? textures[std::size_t(index)].minFilter : 0,
            index >= 0 && static_cast<std::size_t>(index) < textures.size()
                ? textures[std::size_t(index)].wrapS : 0,
            index >= 0 && static_cast<std::size_t>(index) < textures.size()
                ? textures[std::size_t(index)].wrapT : 0,
            index >= 0 && static_cast<std::size_t>(index) < textures.size()
                ? textures[std::size_t(index)].width : 0,
        };
        append(out, key, sizeof(key));
    };

    std::string key;
    key.reserve(192);
    const int channel = unlitChannel ? 1 : 0;
    append(key, &channel, sizeof(channel));
    const int flags = (primitive.doubleSided ? 1 : 0) | (primitive.alphaBlend ? 2 : 0) |
        (primitive.alphaMask ? 4 : 0) | (primitive.normalMappingEnabled ? 8 : 0) |
        (primitive.topology == ModelPrimitiveTopology::Points ? 16 : 0);
    append(key, &flags, sizeof(flags));
    const int tangent = static_cast<int>(primitive.tangentSource);
    append(key, &tangent, sizeof(tangent));
    const float colours[4] = {static_cast<float>(primitive.baseColorR),
                              static_cast<float>(primitive.baseColorG),
                              static_cast<float>(primitive.baseColorB),
                              static_cast<float>(primitive.baseColorA)};
    append(key, colours, sizeof(colours));
    const float factors[4] = {static_cast<float>(primitive.metallicFactor),
                              static_cast<float>(primitive.roughnessFactor),
                              static_cast<float>(primitive.alphaCutoff),
                              static_cast<float>(primitive.normalScale)};
    append(key, factors, sizeof(factors));
    const float emissive[3] = {static_cast<float>(primitive.emissiveR),
                               static_cast<float>(primitive.emissiveG),
                               static_cast<float>(primitive.emissiveB)};
    append(key, emissive, sizeof(emissive));
    appendIndex(key, primitive.baseColorTextureIndex, primitive.baseColorTexCoord);
    appendIndex(key, primitive.metallicRoughnessTextureIndex, primitive.metallicRoughnessTexCoord);
    appendIndex(key, primitive.normalTextureIndex, primitive.normalTexCoord);
    appendIndex(key, primitive.occlusionTextureIndex, primitive.occlusionTexCoord);
    appendIndex(key, primitive.emissiveTextureIndex, primitive.emissiveTexCoord);
    return key;
}

} // namespace

OgreScene::OgreScene(OgreRuntime& runtime) : m_runtime(runtime), m_root(runtime.root()) {}

OgreScene::~OgreScene()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_alive->store(false);
    m_layerStateCallback = {};
    shutdown();
}

bool OgreScene::ensureReady(const QString& runtimeDirectory)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_ready)
        return true;
    m_root = m_runtime.root();
    if (!m_root || !m_runtime.ensureHlms(runtimeDirectory))
        return false;
    try {
        if (!m_sceneManager) {
            m_sceneManager = m_root->createSceneManager(
                Ogre::ST_GENERIC, 1u,
                "View3DScene" + Ogre::StringConverter::toString(nextOgreResourceSerial()));
        }
        if (!createLightingResources() || !applyLightingStyle() ||
            !rebuildEnvironmentObjects())
            return false;
        if (m_globeMode && !createGlobeSurface())
            return false;
        // 环境反射：金属/玻璃没有 IBL 时只能渲染成近黑。这里用一张"只有天空球"
        // 的静态探针给 PBS 提供反射；VIEW3D_PBS_REFLECTION=0 可关闭。
        const QString reflectionSetting = qEnvironmentVariable("VIEW3D_PBS_REFLECTION");
        if (reflectionSetting.isEmpty() || reflectionSetting != QStringLiteral("0")) {
            m_pbsReflection = std::make_shared<PbsReflection>(m_root, m_sceneManager);
            if (!m_pbsReflection->initialize())
                m_pbsReflection.reset();
        }
        startPreprocessors();
        m_ready = true;
        std::cerr << "VIEW3D_OGRE_STAGE: scene_ready\n";
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_OGRE_SCENE_FAILED: " << error.getFullDescription() << "\n";
        shutdown();
        return false;
    }
}

void OgreScene::shutdown()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_pbsReflection) {
        m_pbsReflection->shutdown();
        m_pbsReflection.reset();
    }
    stopPreprocessors();
    m_uploadQueue.clear();
    m_viewSlots.clear();
    m_tileGroups.clear();
    {
        std::lock_guard<std::mutex> preparedLock(m_preparedMutex);
        m_preparedUploads.clear();
    }
    clearModelLayers();
    if (m_root) {
        auto* textureManager = m_root->getRenderSystem()->getTextureGpuManager();
        for (auto& entry : m_textureCache) {
            if (entry.second.texture)
                textureManager->destroyTexture(entry.second.texture);
        }
    }
    m_textureCache.clear();
    m_textureKeys.clear();
    clearTileDebugOverlay();
    clearEnvironmentObjects();
    clearGlobeSurface();
    m_globeMode = false;
    m_globeAnchorValid = false;
    m_globeRenderOrigin = {};
    destroyLightingResources();
    if (m_root && m_sceneManager)
        m_root->destroySceneManager(m_sceneManager);
    m_sceneManager = nullptr;
    m_mainLight = nullptr;
    m_mainLightNode = nullptr;
    m_root = nullptr;
    m_ready = false;
}

bool OgreScene::ready() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_ready && m_sceneManager != nullptr;
}

void OgreScene::processPendingUploads()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_ready)
        return;
    pruneOrphanPreparations();
    QElapsedTimer timer;
    timer.start();
    QList<QString> deferred;
    bool uploaded = false;
    ++m_uploadFrames;
    while (!m_uploadQueue.isEmpty()) {
        if (timer.elapsed() >= 8) {
            ++m_uploadBudgetHits; // 这一帧是被 8 ms 预算掐断的，不是队列空了
            break;
        }
        const QString key = m_uploadQueue.dequeue();
        const auto it = m_layers.find(key);
        if (it == m_layers.end() || !it->second.pending)
            continue;
        PreparedUpload prepared;
        if (!takePrepared(key, prepared)) {
            deferred.append(key);
            continue;
        }
        if (prepared.sourceMesh != it->second.sourceMesh) {
            // 排队期间图层换了几何（动态投影重放）：这份预处理结果属于上一版网格，
            // 丢掉即可——新网格的预处理在替换时已经另行入队。
            deferred.append(key);
            continue;
        }
        if (prepared.failed) {
            it->second.pending = false;
            it->second.failed = true;
            notifyLayerState(key, LayerRenderState::Failed);
            continue;
        }
        QElapsedTimer oneUpload;
        oneUpload.start();
        if (uploadLayer(key, prepared)) {
            uploaded = true;
            m_uploadUs += static_cast<std::uint64_t>(oneUpload.nsecsElapsed() / 1000);
            ++m_uploadCount;
            if (!qEnvironmentVariable("VIEW3D_UPLOAD_STATS").isEmpty() &&
                m_uploadCount % 64 == 0) {
                const std::uint64_t preCount =
                    m_preprocessCount.load(std::memory_order_relaxed);
                const std::uint64_t preUs = m_preprocessUs.load(std::memory_order_relaxed);
                std::cerr << "VIEW3D_UPLOAD uploads=" << m_uploadCount
                          << " upload_avg_ms=" << (m_uploadCount ? double(m_uploadUs) /
                                                                        double(m_uploadCount) / 1000.0 : 0.0)
                          << " preprocess_n=" << preCount
                          << " preprocess_avg_ms="
                          << (preCount ? double(preUs) / double(preCount) / 1000.0 : 0.0)
                          << " frames=" << m_uploadFrames << " budget_hits=" << m_uploadBudgetHits
                          << " queued=" << m_uploadQueue.size() << std::endl << std::flush;
            }
        }
    }
    for (const QString& key : deferred)
        m_uploadQueue.enqueue(key);
    if (uploaded && m_tileDebugEnabled)
        rebuildTileDebugOverlay();
}

bool OgreScene::addModelLayer(const QString& layerKey,
                              std::shared_ptr<const ModelMeshDto> mesh)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return addLayerInternal(layerKey, std::move(mesh), false, {});
}

bool OgreScene::addTile(const QString& tileGroupKey, const QString& tileKey,
                        std::shared_ptr<const ModelMeshDto> mesh)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty() || tileKey.isEmpty())
        return false;
    return addLayerInternal(
        QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey), std::move(mesh), true,
        tileGroupKey);
}

bool OgreScene::addLayerInternal(const QString& layerKey,
                                 std::shared_ptr<const ModelMeshDto> mesh, bool tile,
                                 const QString& tileGroup)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (layerKey.isEmpty() || !mesh || mesh->vertices.empty() || mesh->primitives.empty())
        return false;
    if (m_layers.find(layerKey) != m_layers.end())
        return false;
    if (m_renderBudgetMaxVertices > 0 &&
        totalRegisteredVertices() + mesh->vertices.size() > m_renderBudgetMaxVertices)
        return false;
    ModelResources resources;
    resources.sourceMesh = std::move(mesh);
    resources.pickMesh = resources.sourceMesh;
    resources.tile = tile;
    resources.tileGroup = tileGroup;
    if (tile) {
        const auto group = m_tileGroups.find(tileGroup);
        if (group != m_tileGroups.end()) {
            resources.userVisible = group->second.visible;
            resources.groupOpacity = group->second.opacity;
            resources.groupOffset = group->second.offset;
            // 流式加载期间切过一次投影时，新到的瓦片要继承已经生效的摆放。
            resources.groupYawDegrees = group->second.yawDegrees;
            resources.groupHorizontalScale = group->second.horizontalScale;
            resources.groupPlacement = group->second.placement;
        }
    }
    if (m_globeMode && resources.sourceMesh->hasContentOrigin) {
        resources.globeAnchored = true;
        resources.globeBasePosition =
            GlobeMath::sub(resources.sourceMesh->contentOrigin, m_globeRenderOrigin);
    }
    resources.pending = true;
    m_layers.emplace(layerKey, std::move(resources));
    m_uploadQueue.enqueue(layerKey);
    startPreprocessing(layerKey, m_layers.find(layerKey)->second.sourceMesh);
    notifyLayerState(layerKey, LayerRenderState::Pending);
    return true;
}

bool OgreScene::uploadLayer(const QString& layerKey, const PreparedUpload& prepared)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end() || !it->second.pending || !it->second.sourceMesh)
        return false;
    ModelResources state = it->second;
    if (!buildCurrentModel(*state.sourceMesh, prepared)) {
        it->second.pending = false;
        it->second.failed = true;
        notifyLayerState(layerKey, LayerRenderState::Failed);
        return false;
    }
    ModelResources built = takeCurrentModel();
    built.sourceMesh = std::move(state.sourceMesh);
    built.pickMesh = built.sourceMesh;
    built.tile = state.tile;
    built.tileGroup = state.tileGroup;
    built.visible = state.visible;
    // 上传期间用户（或 LOD 调度）可能已经改过这些状态：漏拷一项就会在上传完成时
    // 被静默重置，瓦片调度里表现为粗父瓦片的隐藏失效、与子瓦片同时渲染。
    built.userVisible = state.userVisible;
    built.groupOpacity = state.groupOpacity;
    built.groupOffset = state.groupOffset;
    built.groupYawDegrees = state.groupYawDegrees;
    built.groupHorizontalScale = state.groupHorizontalScale;
    built.groupPlacement = state.groupPlacement;
    built.globeAnchored = state.globeAnchored;
    built.globeBasePosition = state.globeBasePosition;
    built.distanceLimited = state.distanceLimited;
    built.opacity = state.opacity;
    built.minimumDistance = state.minimumDistance;
    built.maximumDistance = state.maximumDistance;
    built.offset = state.offset;
    built.tint = state.tint;
    built.edgeLinesVisible = state.edgeLinesVisible;
    built.brightness = state.brightness;
    built.contrast = state.contrast;
    built.selectedFirstPrimitive = state.selectedFirstPrimitive;
    built.selectedPrimitiveCount = state.selectedPrimitiveCount;
    built.nodeMask = state.nodeMask;
    built.pending = false;
    it->second = std::move(built);
    applyLayerState(it->first, it->second);
    if (it->second.distanceLimited)
        updateDistanceVisibility();
    notifyLayerState(layerKey, LayerRenderState::Ready);
    return true;
}

void OgreScene::startPreprocessors()
{
    if (!m_preprocessThreads.empty())
        return;
    unsigned cores = std::thread::hardware_concurrency();
    if (cores < 2u)
        cores = 2u;
    const std::size_t count = qMin<std::size_t>(4u, qMax<std::size_t>(1u, cores / 2u));
    {
        std::lock_guard<std::mutex> lock(m_preprocessMutex);
        m_preprocessStopping = false;
    }
    m_preprocessThreads.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        m_preprocessThreads.emplace_back([this] { preprocessWorker(); });
}

void OgreScene::stopPreprocessors()
{
    {
        std::lock_guard<std::mutex> lock(m_preprocessMutex);
        m_preprocessStopping = true;
        m_preprocessJobs.clear();
    }
    m_preprocessCondition.notify_all();
    for (std::thread& worker : m_preprocessThreads) {
        if (worker.joinable())
            worker.join();
    }
    m_preprocessThreads.clear();
}

void OgreScene::preprocessWorker()
{
    while (true) {
        PreprocessJob job;
        {
            std::unique_lock<std::mutex> lock(m_preprocessMutex);
            m_preprocessCondition.wait(lock, [this] {
                return m_preprocessStopping || !m_preprocessJobs.empty();
            });
            if (m_preprocessStopping)
                return;
            job = std::move(m_preprocessJobs.front());
            m_preprocessJobs.pop_front();
        }
        PreparedUpload prepared;
        QElapsedTimer preTimer;
        preTimer.start();
        try {
            prepared = preprocessMesh(job.mesh);
        } catch (const std::exception& error) {
            std::cerr << "VIEW3D_UPLOAD_PREPROCESS_FAILED: " << error.what() << "\n";
            prepared = PreparedUpload {};
            prepared.sourceMesh = job.mesh;
            prepared.failed = true;
        } catch (...) {
            std::cerr << "VIEW3D_UPLOAD_PREPROCESS_FAILED: unknown\n";
            prepared = PreparedUpload {};
            prepared.sourceMesh = job.mesh;
            prepared.failed = true;
        }
        m_preprocessUs += static_cast<std::uint64_t>(preTimer.nsecsElapsed() / 1000);
        ++m_preprocessCount;
        std::lock_guard<std::mutex> lock(m_preparedMutex);
        m_preparedUploads[job.layerKey] = std::move(prepared);
    }
}

void OgreScene::startPreprocessing(const QString& layerKey,
                                   std::shared_ptr<const ModelMeshDto> mesh)
{
    if (layerKey.isEmpty() || !mesh)
        return;
    {
        std::lock_guard<std::mutex> lock(m_preprocessMutex);
        if (m_preprocessStopping)
            return;
        m_preprocessJobs.push_back(PreprocessJob {layerKey, std::move(mesh)});
    }
    m_preprocessCondition.notify_one();
}

bool OgreScene::takePrepared(const QString& layerKey, PreparedUpload& out)
{
    std::lock_guard<std::mutex> lock(m_preparedMutex);
    const auto it = m_preparedUploads.find(layerKey);
    if (it == m_preparedUploads.end())
        return false;
    out = std::move(it->second);
    m_preparedUploads.erase(it);
    return true;
}

void OgreScene::pruneOrphanPreparations()
{
    std::lock_guard<std::mutex> lock(m_preparedMutex);
    for (auto it = m_preparedUploads.begin(); it != m_preparedUploads.end();) {
        const auto layer = m_layers.find(it->first);
        if (layer == m_layers.end() || !layer->second.pending)
            it = m_preparedUploads.erase(it);
        else
            ++it;
    }
}

std::vector<OgreScene::TextureRequest> OgreScene::collectTextureRequests(
    const ModelMeshDto& mesh)
{
    std::vector<TextureRequest> requests;
    std::set<std::tuple<std::int32_t, std::uint8_t, std::uint16_t, bool>> seen;
    const auto add = [&](std::int32_t sourceIndex, std::uint8_t channel,
                         std::uint16_t strengthKey, bool srgb) {
        if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >= mesh.textures.size())
            return;
        if (!seen.emplace(sourceIndex, channel, strengthKey, srgb).second)
            return;
        TextureRequest request;
        request.sourceIndex = sourceIndex;
        request.channel = channel;
        request.strengthKey = strengthKey;
        request.srgb = srgb;
        requests.push_back(request);
    };
    for (const ModelPrimitiveDto& primitive : mesh.primitives) {
        add(primitive.baseColorTextureIndex, 4u, 1000u, true);
        add(primitive.emissiveTextureIndex, 4u, 1000u, true);
        if (primitive.metallicRoughnessTextureIndex >= 0) {
            add(primitive.metallicRoughnessTextureIndex, 2u, 1000u, false);
            add(primitive.metallicRoughnessTextureIndex, 1u, 1000u, false);
        }
        if (primitive.normalMappingEnabled)
            add(primitive.normalTextureIndex, 4u, 1000u, false);
        if (primitive.occlusionTextureIndex >= 0) {
            const std::uint16_t strengthKey = static_cast<std::uint16_t>(qBound(
                0, qRound(static_cast<double>(primitive.occlusionStrength) * 1000.0), 1000));
            add(primitive.occlusionTextureIndex, 0u, strengthKey, false);
        }
    }
    return requests;
}

OgreScene::PreparedTexture OgreScene::preprocessTexture(const ModelTextureDto& source,
                                                        const TextureRequest& request)
{
    PreparedTexture item;
    item.sourceIndex = request.sourceIndex;
    item.channel = request.channel;
    item.strengthKey = request.strengthKey;
    item.srgb = request.srgb;
    item.width = source.width;
    item.height = source.height;
    if (source.width <= 0 || source.height <= 0)
        return item;
    const std::size_t pixels = static_cast<std::size_t>(source.width) *
        static_cast<std::size_t>(source.height);
    // 提交阶段按 width*height*4 读取缓冲，先在这里校验，避免越界读。
    if (source.rgba8.size() < pixels * 4u)
        return item;
    const bool needsChannels = !request.srgb && request.channel < 4u;
    if (needsChannels) {
        item.rgba.resize(pixels * 4u);
        const double strength = request.strengthKey / 1000.0;
        for (std::size_t pixel = 0; pixel + 3u < source.rgba8.size(); pixel += 4u) {
            const int sourceValue = std::to_integer<int>(source.rgba8[pixel + request.channel]);
            const int value = request.channel == 0u && request.strengthKey < 1000u
                ? qBound(0, qRound(255.0 + strength * (sourceValue - 255.0)), 255)
                : sourceValue;
            item.rgba[pixel] = std::byte(value);
            item.rgba[pixel + 1u] = std::byte(value);
            item.rgba[pixel + 2u] = std::byte(value);
            item.rgba[pixel + 3u] = std::byte(255);
        }
    } else {
        item.rgba.assign(source.rgba8.begin(), source.rgba8.begin() +
            static_cast<std::ptrdiff_t>(pixels * 4u));
    }
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromRawData(reinterpret_cast<const char*>(item.rgba.data()),
                                static_cast<int>(item.rgba.size())),
        QCryptographicHash::Sha1);
    item.cacheKey = digest.toHex().toStdString() + (request.srgb ? ":srgb:" : ":linear:") +
        std::to_string(source.width) + "x" + std::to_string(source.height);
    item.valid = true;
    return item;
}

OgreScene::PreparedUpload OgreScene::preprocessMesh(
    const std::shared_ptr<const ModelMeshDto>& mesh)
{
    PreparedUpload prepared;
    prepared.sourceMesh = mesh;
    if (!mesh || mesh->vertices.empty() || mesh->primitives.empty()) {
        prepared.failed = true;
        return prepared;
    }
    for (const TextureRequest& request : collectTextureRequests(*mesh))
        prepared.textures.push_back(
            preprocessTexture(mesh->textures[static_cast<std::size_t>(request.sourceIndex)],
                              request));
    // 世界包围盒：与提交阶段一致，取首个包含该 primitive 的节点变换。
    std::vector<std::int32_t> ownerNode(mesh->primitives.size(), -1);
    for (std::size_t nodeIndex = 0; nodeIndex < mesh->nodes.size(); ++nodeIndex) {
        const ModelNodeDto& node = mesh->nodes[nodeIndex];
        if (node.firstPrimitive > mesh->primitives.size())
            continue;
        const std::size_t end = qMin<std::size_t>(
            mesh->primitives.size(),
            static_cast<std::size_t>(node.firstPrimitive) + node.primitiveCount);
        for (std::size_t primitiveIndex = node.firstPrimitive; primitiveIndex < end;
             ++primitiveIndex) {
            if (ownerNode[primitiveIndex] < 0)
                ownerNode[primitiveIndex] = static_cast<std::int32_t>(nodeIndex);
        }
    }
    bool initialised = false;
    std::array<double, 3> minimum {std::numeric_limits<double>::max(),
                                   std::numeric_limits<double>::max(),
                                   std::numeric_limits<double>::max()};
    std::array<double, 3> maximum {std::numeric_limits<double>::lowest(),
                                   std::numeric_limits<double>::lowest(),
                                   std::numeric_limits<double>::lowest()};
    for (std::size_t primitiveIndex = 0; primitiveIndex < mesh->primitives.size();
         ++primitiveIndex) {
        const ModelPrimitiveDto& primitive = mesh->primitives[primitiveIndex];
        if (primitive.firstVertex > mesh->vertices.size() ||
            primitive.vertexCount > mesh->vertices.size() - primitive.firstVertex)
            continue;
        const ModelNodeDto* node = ownerNode[primitiveIndex] >= 0
            ? &mesh->nodes[static_cast<std::size_t>(ownerNode[primitiveIndex])]
            : nullptr;
        for (std::uint32_t vertexIndex = 0; vertexIndex < primitive.vertexCount; ++vertexIndex) {
            const MeshVertexDto& vertex = mesh->vertices[primitive.firstVertex + vertexIndex];
            double x = vertex.x, y = vertex.y, z = vertex.z;
            if (node) {
                const auto& m = node->worldTransform;
                x = m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12];
                y = m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13];
                z = m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14];
            }
            minimum[0] = qMin(minimum[0], x);
            minimum[1] = qMin(minimum[1], y);
            minimum[2] = qMin(minimum[2], z);
            maximum[0] = qMax(maximum[0], x);
            maximum[1] = qMax(maximum[1], y);
            maximum[2] = qMax(maximum[2], z);
            initialised = true;
        }
    }
    prepared.hasBounds = initialised;
    prepared.boundsMinimum = minimum;
    prepared.boundsMaximum = maximum;
    std::size_t hashedBytes = 0;
    for (const PreparedTexture& item : prepared.textures)
        hashedBytes += item.rgba.size();
    std::cerr << "VIEW3D_OGRE_STAGE: preprocess thread="
              << std::hash<std::thread::id>()(std::this_thread::get_id())
              << " textures=" << prepared.textures.size()
              << " bytes=" << hashedBytes << "\n";
    return prepared;
}

void OgreScene::applyLayerState(const QString& layerKey, ModelResources& resources)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!resources.rootNode)
        return;
    applyLayerPosition(resources);
    applyDisplayMode(resources);
    applyLayerOpacity(resources);
    setModelLayerStyle(layerKey, resources.tint, resources.edgeLinesVisible,
                       resources.brightness, resources.contrast);
    if (resources.selectedPrimitiveCount > 0)
        selectModelLayerRange(layerKey, resources.selectedFirstPrimitive,
                              resources.selectedPrimitiveCount);
    // 用户可见性走节点开关，LOD 与距离裁剪走可见性位，两者互不覆盖。
    resources.rootNode->setVisible(effectiveVisible(resources), true);
    applyNodeVisibility(resources);
}

bool OgreScene::updateModelLayerMesh(const QString& layerKey,
                                     std::shared_ptr<const ModelMeshDto> mesh)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end() || !mesh || mesh->vertices.empty() || mesh->primitives.empty())
        return false;
    // 只取状态位：Ogre 对象指针在 remove 时会被销毁，这份副本不能用来访问渲染资源。
    const ModelResources previous = it->second;
    if (!removeModelLayer(layerKey))
        return false;
    if (!addLayerInternal(layerKey, mesh, previous.tile, previous.tileGroup))
        return false;
    // 重放不该让图层闪回默认值：用户通道的每一项都要原样搬回新的 pending 记录，
    // 上传完成时 uploadLayer 会再按这些字段重建资源。
    ModelResources& resources = m_layers.find(layerKey)->second;
    resources.visible = previous.visible;
    resources.userVisible = previous.userVisible;
    resources.groupOpacity = previous.groupOpacity;
    resources.groupOffset = previous.groupOffset;
    resources.groupYawDegrees = previous.groupYawDegrees;
    resources.groupHorizontalScale = previous.groupHorizontalScale;
    resources.groupPlacement = previous.groupPlacement;
    resources.distanceLimited = previous.distanceLimited;
    resources.opacity = previous.opacity;
    resources.minimumDistance = previous.minimumDistance;
    resources.maximumDistance = previous.maximumDistance;
    resources.offset = previous.offset;
    resources.tint = previous.tint;
    resources.edgeLinesVisible = previous.edgeLinesVisible;
    resources.brightness = previous.brightness;
    resources.contrast = previous.contrast;
    resources.nodeMask = previous.nodeMask;
    resources.selectedFirstPrimitive = previous.selectedFirstPrimitive;
    resources.selectedPrimitiveCount = previous.selectedPrimitiveCount;
    if (resources.distanceLimited)
        ++m_distanceLimitedCount;   // removeModelLayer 里减过一次
    return true;
}

bool OgreScene::removeModelLayer(const QString& layerKey)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    if (it->second.distanceLimited && m_distanceLimitedCount > 0)
        --m_distanceLimitedCount;
    clearFeatureHighlight(it->second);
    installCurrentModel(std::move(it->second));
    m_layers.erase(it);
    clearCurrentModel();
    if (m_tileDebugEnabled && layerKey.startsWith(QStringLiteral("tile:")))
        rebuildTileDebugOverlay();
    return true;
}

void OgreScene::clearModelLayers()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_distanceLimitedCount = 0;
    while (!m_layers.empty()) {
        auto it = m_layers.begin();
        clearFeatureHighlight(it->second);
        installCurrentModel(std::move(it->second));
        m_layers.erase(it);
        clearCurrentModel();
    }
}

bool OgreScene::setModelLayerVisible(const QString& layerKey, bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    ModelResources& resources = it->second;
    resources.visible = visible;
    if (resources.rootNode) {
        resources.rootNode->setVisible(effectiveVisible(resources), true);
        applyNodeVisibility(resources);
    }
    return true;
}

bool OgreScene::isModelLayerVisible(const QString& layerKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    return it != m_layers.end() && it->second.visible;
}

std::uint32_t OgreScene::tileCount(const QString& tileGroupKey) const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint32_t count = 0;
    for (const auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (resources.tile && resources.tileGroup == tileGroupKey)
            ++count;
    }
    return count;
}

bool OgreScene::removeTile(const QString& tileGroupKey, const QString& tileKey)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return removeModelLayer(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey));
}

bool OgreScene::setTileVisible(const QString& tileGroupKey, const QString& tileKey,
                               bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return setModelLayerVisible(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey),
                                visible);
}

bool OgreScene::setTileNodeMask(const QString& tileGroupKey, const QString& tileKey,
                                std::uint64_t nodeMask)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const QString layerKey = QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey);
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    it->second.nodeMask = nodeMask;
    if (it->second.rootNode)
        applyNodeVisibility(it->second);
    return true;
}

bool OgreScene::isTileVisible(const QString& tileGroupKey, const QString& tileKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return isModelLayerVisible(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey));
}

LayerRenderState OgreScene::tileRenderState(const QString& tileGroupKey,
                                            const QString& tileKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return modelLayerRenderState(
        QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey));
}

bool OgreScene::setTileOpacity(const QString& tileGroupKey, const QString& tileKey,
                               float opacity)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return setModelLayerOpacity(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey),
                                opacity);
}

bool OgreScene::setTileOffset(const QString& tileGroupKey, const QString& tileKey,
                              const std::array<double, 3>& offset)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return setModelLayerOffset(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey),
                               offset);
}

float OgreScene::tileOpacity(const QString& tileGroupKey, const QString& tileKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return modelLayerOpacity(QStringLiteral("tile:%1/%2").arg(tileGroupKey, tileKey));
}

bool OgreScene::tilesHaveOpacity(const QString& tileGroupKey, float opacity) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    for (const auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (resources.tile && resources.tileGroup == tileGroupKey &&
            qAbs(resources.opacity - opacity) > 1e-4f)
            return false;
    }
    return true;
}

bool OgreScene::tilesUseTransparency(const QString& tileGroupKey, bool transparent) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    for (const auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (resources.tile && resources.tileGroup == tileGroupKey) {
            const bool hasTransparency = resources.opacity < 0.9999f ||
                std::any_of(resources.alphaBlended.begin(), resources.alphaBlended.end(),
                            [](bool value) { return value; });
            if (hasTransparency != transparent)
                return false;
        }
    }
    return true;
}

bool OgreScene::setTileGroupVisible(const QString& tileGroupKey, bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty())
        return false;
    m_tileGroups[tileGroupKey].visible = visible;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (!resources.tile || resources.tileGroup != tileGroupKey)
            continue;
        resources.userVisible = visible;
        if (resources.rootNode) {
            resources.rootNode->setVisible(effectiveVisible(resources), true);
            applyNodeVisibility(resources);
        }
    }
    return true;
}

bool OgreScene::setTileGroupOpacity(const QString& tileGroupKey, float opacity)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty() || !qIsFinite(opacity) || opacity < 0.0f || opacity > 1.0f)
        return false;
    const float clamped = qBound(0.0f, opacity, 1.0f);
    m_tileGroups[tileGroupKey].opacity = clamped;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (!resources.tile || resources.tileGroup != tileGroupKey)
            continue;
        resources.groupOpacity = clamped;
        applyLayerOpacity(resources);
    }
    return true;
}

bool OgreScene::setTileGroupOffset(const QString& tileGroupKey,
                                   const std::array<double, 3>& offset)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty())
        return false;
    for (const double component : offset) {
        if (!qIsFinite(component))
            return false;
    }
    m_tileGroups[tileGroupKey].offset = offset;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (!resources.tile || resources.tileGroup != tileGroupKey)
            continue;
        resources.groupOffset = offset;
        applyLayerPosition(resources);
    }
    return true;
}

bool OgreScene::setTileGroupPlaneTransform(const QString& tileGroupKey, double yawDegrees,
                                           double horizontalScale,
                                           const std::array<double, 3>& placementOffset)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty() || !qIsFinite(yawDegrees) || !qIsFinite(horizontalScale) ||
        horizontalScale <= 0.0)
        return false;
    for (const double component : placementOffset) {
        if (!qIsFinite(component))
            return false;
    }
    TileGroupState& group = m_tileGroups[tileGroupKey];
    const bool unchanged = group.yawDegrees == yawDegrees &&
        group.horizontalScale == horizontalScale && group.placement == placementOffset;
    if (unchanged)
        return true;
    group.yawDegrees = yawDegrees;
    group.horizontalScale = horizontalScale;
    group.placement = placementOffset;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (!resources.tile || resources.tileGroup != tileGroupKey)
            continue;
        resources.groupYawDegrees = yawDegrees;
        resources.groupHorizontalScale = horizontalScale;
        resources.groupPlacement = placementOffset;
        applyLayerPosition(resources);
    }
    if (m_distanceLimitedCount > 0)
        updateDistanceVisibility();
    return true;
}

bool OgreScene::clearTileGroup(const QString& tileGroupKey)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (tileGroupKey.isEmpty())
        return false;
    m_tileGroups.erase(tileGroupKey);
    QList<QString> doomed;
    for (const auto& [key, resources] : m_layers) {
        if (resources.tile && resources.tileGroup == tileGroupKey)
            doomed.append(key);
    }
    for (const QString& key : doomed)
        removeModelLayer(key);
    return !doomed.isEmpty();
}

QList<QString> OgreScene::modelLayerKeys() const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    QList<QString> keys;
    keys.reserve(static_cast<qsizetype>(m_layers.size()));
    for (const auto& entry : m_layers)
        keys.append(entry.first);
    return keys;
}

std::uint64_t OgreScene::totalRegisteredVertices() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint64_t count = 0;
    for (const auto& entry : m_layers)
        if (entry.second.sourceMesh)
            count += entry.second.sourceMesh->vertices.size();
    return count;
}

std::uint64_t OgreScene::registeredVertexCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint64_t count = 0;
    for (const auto& entry : m_layers)
        count += entry.second.vertexCount;
    return count;
}

std::uint64_t OgreScene::registeredIndexCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint64_t count = 0;
    for (const auto& entry : m_layers)
        count += entry.second.indexCount;
    return count;
}

std::uint64_t OgreScene::registeredTextureCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return static_cast<std::uint64_t>(m_textureCache.size());
}

std::uint32_t OgreScene::registeredSceneNodeCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint32_t count = 0;
    for (const auto& entry : m_layers) {
        count += static_cast<std::uint32_t>(entry.second.sceneNodes.size());
        if (entry.second.rootNode)
            ++count;
    }
    return count;
}

std::uint32_t OgreScene::registeredRenderObjectCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint32_t count = 0;
    for (const auto& entry : m_layers)
        count += static_cast<std::uint32_t>(entry.second.objects.size());
    return count;
}

void OgreScene::setRenderBudgetMaxVertices(std::uint64_t maximum)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_renderBudgetMaxVertices = maximum;
}

std::uint64_t OgreScene::renderBudgetMaxVertices() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_renderBudgetMaxVertices;
}

std::uint64_t OgreScene::renderBudgetCurrentVertices() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return registeredVertexCount();
}

bool OgreScene::pickRay(const std::array<double, 3>& origin,
                        const std::array<double, 3>& direction,
                        ViewportPickResult* result) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (result)
        *result = {};
    if (!result)
        return false;
    const double lengthSquared = direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(lengthSquared) || lengthSquared < 1e-18)
        return false;
    double nearestTriangle = std::numeric_limits<double>::max();
    double nearestBounds = std::numeric_limits<double>::max();
    ViewportPickResult selected;
    ViewportPickResult fallback;
    bool exactHit = false;
    // 先用缓存包围盒剔除：OSGB 一个数据集有几百个瓦片图层，逐层暴搜三角形
    // 会让滚轮/拖拽期间的拾取卡住交互。
    const auto rayBoundsDistance = [&](const RenderBounds& bounds, double* nearDistance) {
        double minimumDistance = 0.0;
        double maximumDistance = std::numeric_limits<double>::max();
        for (int axis = 0; axis < 3; ++axis) {
            if (qAbs(direction[axis]) < 1e-15) {
                if (origin[axis] < bounds.minimum[axis] ||
                    origin[axis] > bounds.maximum[axis])
                    return false;
                continue;
            }
            double t0 = (bounds.minimum[axis] - origin[axis]) / direction[axis];
            double t1 = (bounds.maximum[axis] - origin[axis]) / direction[axis];
            if (t0 > t1)
                std::swap(t0, t1);
            minimumDistance = qMax(minimumDistance, t0);
            maximumDistance = qMin(maximumDistance, t1);
            if (minimumDistance > maximumDistance)
                return false;
        }
        if (maximumDistance < 0.0)
            return false;
        if (nearDistance)
            *nearDistance = minimumDistance;
        return true;
    };
    for (const auto& [layerKey, resources] : m_layers) {
        if (resources.pending || !resources.visible || !resources.pickMesh)
            continue;
        if (m_globeMode && resources.sourceMesh && !resources.sourceMesh->hasContentOrigin)
            continue;   // 未球面化的图层看不见，也不该点中
        RenderBounds bounds;
        double boundsDistance = 0.0;
        const bool boundsValid =
            modelLayerBounds(layerKey, &bounds) && bounds.valid &&
            rayBoundsDistance(bounds, &boundsDistance);
        if (boundsValid && boundsDistance < nearestBounds) {
            nearestBounds = boundsDistance;
            fallback = {};
            fallback.renderKey = layerKey;
            fallback.hasWorldPosition = false;
        }
        if (!boundsValid)
            continue;
        // 拾取的正反算必须与节点的 setPosition/setOrientation/setScale 同源，
        // 否则投影切换后就是「看得见但点不中」这种最难查的症状。
        const std::array<double, 3> localOrigin = worldToLocalPoint(origin, resources);
        const double pickYaw = resources.placementYawDegrees() * kDegreesToRadians;
        const double pickCos = std::cos(pickYaw);
        const double pickSin = std::sin(pickYaw);
        const double pickScale = resources.placementScale() != 0.0
            ? resources.placementScale()
            : 1.0;
        const std::array<double, 3> localDirection {
            (direction[0] * pickCos - direction[2] * pickSin) / pickScale, direction[1],
            (direction[0] * pickSin + direction[2] * pickCos) / pickScale};
        ModelRayHitDto hit;
        const ModelMeshDto& mesh = *resources.pickMesh;
        bool triangleHit = !mesh.rayNodes.empty()
            ? mesh.rayHitIndexed(localOrigin, localDirection, &hit)
            : mesh.rayHit(localOrigin, localDirection, &hit);
        // 点云/点基元没有三角形可相交：按图层包围盒尺度的 1% 作为拾取
        // 容差（最小 1 mm），让光标下的点成为旋转/缩放枢轴。测距仍只认
        // 三角面，因此 selected.exactTriangle 对点命中保持 false。
        ModelRayHitDto pointHit;
        bool pointFound = false;
        const double diagonalX = bounds.maximum[0] - bounds.minimum[0];
        const double diagonalY = bounds.maximum[1] - bounds.minimum[1];
        const double diagonalZ = bounds.maximum[2] - bounds.minimum[2];
        const double diagonal = std::sqrt(diagonalX * diagonalX + diagonalY * diagonalY +
                                          diagonalZ * diagonalZ);
        const double pointRadius = std::max(1.0e-3, diagonal * 0.01);
        pointFound = mesh.rayHitPoints(localOrigin, localDirection, pointRadius, &pointHit);
        const bool usePoint =
            pointFound && (!triangleHit || pointHit.distance < hit.distance);
        if (usePoint) {
            hit = pointHit;
            triangleHit = false;
        }
        bool exactHitThisLayer = triangleHit || usePoint;
        if (exactHitThisLayer) {
            // 被节点掩码隐藏的几何不参与拾取，否则点到的是看不见的粗父级表面。
            const std::size_t nodeIndex = static_cast<std::size_t>(hit.nodeIndex);
            const std::int32_t sourceIndex =
                nodeIndex < mesh.nodes.size() ? mesh.nodes[nodeIndex].sourceNodeIndex : -1;
            if (sourceIndex >= 0 && sourceIndex < 64 &&
                ((resources.nodeMask >> sourceIndex) & 1ull) == 0ull)
                exactHitThisLayer = false;
        }
        if (exactHitThisLayer && hit.distance < nearestTriangle) {
            nearestTriangle = hit.distance;
            exactHit = true;
            selected.renderKey = layerKey;
            selected.nodeIndex = hit.nodeIndex;
            selected.primitiveIndex = hit.primitiveIndex;
            selected.triangleIndex = hit.triangleIndex;
            selected.worldPosition = localPointToWorld(hit.position, resources);
            selected.triangleVertices = hit.triangleVertices;
            for (auto& vertex : selected.triangleVertices)
                vertex = localPointToWorld(vertex, resources);
            selected.hasTriangleVertices = triangleHit && hit.hasTriangleVertices;
            selected.hasWorldPosition = true;
            selected.exactTriangle = triangleHit;
            selected.hasFeature = hit.hasFeature;
            if (hit.hasFeature) {
                selected.featureTableIndex = hit.feature.tableIndex;
                selected.featureId = hit.feature.featureId;
                selected.featureProperties = hit.feature.properties;
            }
        }
    }
    if (exactHit) {
        *result = std::move(selected);
        return true;
    }
    if (!fallback.renderKey.isEmpty()) {
        *result = std::move(fallback);
        return true;
    }
    return false;
}

bool OgreScene::visibleBounds(RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!bounds)
        return false;
    RenderBounds result;
    bool any = false;
    for (const auto& [key, resources] : m_layers) {
        if (!resources.visible)
            continue;
        if (m_globeMode && resources.sourceMesh && !resources.sourceMesh->hasContentOrigin)
            continue;   // 未球面化的图层不在可见范围内
        RenderBounds layer;
        if (!modelLayerBounds(key, &layer) || !layer.valid)
            continue;
        if (!any) {
            result = layer;
            any = true;
        } else {
            for (int axis = 0; axis < 3; ++axis) {
                result.minimum[axis] = qMin(result.minimum[axis], layer.minimum[axis]);
                result.maximum[axis] = qMax(result.maximum[axis], layer.maximum[axis]);
            }
        }
    }
    if (any) {
        result.valid = true;
        *bounds = result;
    }
    return any;
}

bool OgreScene::modelLayerBounds(const QString& layerKey, RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return layerBounds(layerKey, bounds);
}

bool OgreScene::layerBounds(const QString& layerKey, RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!bounds)
        return false;
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    const ModelResources& resources = it->second;
    std::array<double, 3> localMinimum {};
    std::array<double, 3> localMaximum {};
    if (resources.hasBounds) {
        localMinimum = resources.boundsMinimum;
        localMaximum = resources.boundsMaximum;
    } else {
        if (!resources.sourceMesh)
            return false;
        RenderBounds local;
        if (!meshBounds(*resources.sourceMesh, std::array<double, 3> {}, &local) || !local.valid)
            return false;
        localMinimum = local.minimum;
        localMaximum = local.maximum;
    }
    placeLocalBounds(localMinimum, localMaximum, resources, bounds);
    return bounds->valid;
}

bool OgreScene::meshBounds(const ModelMeshDto& mesh,
                           const std::array<double, 3>& offset,
                           RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!bounds || mesh.vertices.empty())
        return false;
    RenderBounds result;
    result.minimum.fill(std::numeric_limits<double>::max());
    result.maximum.fill(std::numeric_limits<double>::lowest());
    const auto include = [&result](const std::array<double, 3>& point) {
        for (int axis = 0; axis < 3; ++axis) {
            result.minimum[axis] = qMin(result.minimum[axis], point[axis]);
            result.maximum[axis] = qMax(result.maximum[axis], point[axis]);
        }
    };
    if (mesh.nodes.empty()) {
        for (const MeshVertexDto& vertex : mesh.vertices)
            include({vertex.x, vertex.y, vertex.z});
    } else {
        for (const ModelNodeDto& node : mesh.nodes) {
            const std::uint32_t first = node.firstPrimitive;
            const std::uint32_t end = first + node.primitiveCount;
            for (std::uint32_t primitiveIndex = first; primitiveIndex < end &&
                 primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
                const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
                for (std::uint32_t index = primitive.firstIndex;
                     index + 2 < primitive.firstIndex + primitive.indexCount; index += 3) {
                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        const std::uint32_t vertexIndex = mesh.indices[index + corner];
                        if (vertexIndex >= mesh.vertices.size())
                            continue;
                        const MeshVertexDto& vertex = mesh.vertices[vertexIndex];
                        const auto& m = node.worldTransform;
                        include({
                            m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
                            m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
                            m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]});
                    }
                }
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        result.minimum[axis] += offset[axis];
        result.maximum[axis] += offset[axis];
    }
    result.valid = result.minimum[0] <= result.maximum[0];
    if (result.valid)
        *bounds = result;
    return result.valid;
}

bool OgreScene::modelLayerNodeBounds(const QString& layerKey, std::uint32_t nodeIndex,
                                     RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end() || !it->second.sourceMesh || !bounds)
        return false;
    RenderBounds local;
    if (!nodeBounds(*it->second.sourceMesh, nodeIndex, std::array<double, 3> {}, &local) ||
        !local.valid)
        return false;
    placeLocalBounds(local.minimum, local.maximum, it->second, bounds);
    return bounds->valid;
}

bool OgreScene::nodeBounds(const ModelMeshDto& mesh, std::uint32_t nodeIndex,
                           const std::array<double, 3>& offset, RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!bounds || nodeIndex >= mesh.nodes.size())
        return false;
    RenderBounds result;
    result.minimum.fill(std::numeric_limits<double>::max());
    result.maximum.fill(std::numeric_limits<double>::lowest());
    const ModelNodeDto& node = mesh.nodes[nodeIndex];
    const auto& m = node.worldTransform;
    for (std::uint32_t primitiveIndex = node.firstPrimitive;
         primitiveIndex < node.firstPrimitive + node.primitiveCount &&
         primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
        const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
        for (std::uint32_t index = primitive.firstIndex;
             index + 2 < primitive.firstIndex + primitive.indexCount; index += 3) {
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t vertexIndex = mesh.indices[index + corner];
                if (vertexIndex >= mesh.vertices.size())
                    continue;
                const MeshVertexDto& vertex = mesh.vertices[vertexIndex];
                const std::array<double, 3> point {
                    m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
                    m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
                    m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
                for (int axis = 0; axis < 3; ++axis) {
                    result.minimum[axis] = qMin(result.minimum[axis], point[axis]);
                    result.maximum[axis] = qMax(result.maximum[axis], point[axis]);
                }
            }
        }
    }
    if (result.minimum[0] > result.maximum[0])
        return false;
    for (int axis = 0; axis < 3; ++axis) {
        result.minimum[axis] += offset[axis];
        result.maximum[axis] += offset[axis];
    }
    result.valid = true;
    *bounds = result;
    return true;
}

bool OgreScene::tileGroupBounds(const QString& tileGroupKey, RenderBounds* bounds) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!bounds)
        return false;
    RenderBounds result;
    bool any = false;
    for (const auto& [key, resources] : m_layers) {
        if (!resources.tile || resources.tileGroup != tileGroupKey)
            continue;
        RenderBounds layer;
        if (!layerBounds(key, &layer) || !layer.valid)
            continue;
        if (!any) {
            result = layer;
            any = true;
        } else {
            for (int axis = 0; axis < 3; ++axis) {
                result.minimum[axis] = qMin(result.minimum[axis], layer.minimum[axis]);
                result.maximum[axis] = qMax(result.maximum[axis], layer.maximum[axis]);
            }
        }
    }
    if (any) {
        result.valid = true;
        *bounds = result;
    }
    return any;
}

void OgreScene::updateDistanceVisibility()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_distanceLimitedCount == 0)
        return;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (!resources.rootNode || !resources.distanceLimited)
            continue;
        // 逐视口判定：只把通过距离测试的视口位写进对象的可见性掩码。
        std::uint32_t mask = 0u;
        const Ogre::Vector3 position = resources.rootNode->getPosition();
        for (const auto& [viewBit, slot] : m_viewSlots) {
            if (!slot.camera)
                continue;
            const Ogre::Vector3 cameraPosition = slot.camera->getDerivedPosition();
            const double dx = position.x - cameraPosition.x;
            const double dy = position.y - cameraPosition.y;
            const double dz = position.z - cameraPosition.z;
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool inside = (resources.minimumDistance <= 0.0 ||
                                 distance >= resources.minimumDistance) &&
                (resources.maximumDistance <= 0.0 || distance <= resources.maximumDistance);
            if (inside)
                mask |= viewBit;
        }
        const std::uint32_t previous = resources.objects.empty()
            ? allViewsMask()
            : resources.objects.front()->getVisibilityFlags();
        applyLayerViewMask(resources, mask);
        if (previous != mask && !qEnvironmentVariable("VIEW3D_VIEW_CULL").isEmpty()) {
            std::cerr << "VIEW3D_VIEW_CULL layer=" << key.toStdString() << " mask=0x"
                      << std::hex << mask << std::dec << " views=" << m_viewSlots.size()
                      << std::endl;
        }
    }
}

std::uint32_t OgreScene::allViewsMask() noexcept
{
    return Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
}

void OgreScene::applyLayerViewMask(const ModelResources& resources, std::uint32_t viewMask)
{
    const auto apply = [&](Ogre::ManualObject* object) {
        if (object && object->getVisibilityFlags() != viewMask)
            object->setVisibilityFlags(viewMask);
    };
    for (Ogre::ManualObject* object : resources.objects)
        apply(object);
    apply(resources.edgeOverlayObject);
    apply(resources.featureHighlightObject);
}

std::uint32_t OgreScene::acquireViewSlot(Ogre::Camera* camera)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint32_t used = 0u;
    for (const auto& [viewBit, slot] : m_viewSlots)
        used |= viewBit;
    const std::uint32_t reserved = allViewsMask();
    for (std::uint32_t viewBit = 1u; viewBit; viewBit <<= 1u) {
        if ((reserved & viewBit) == 0u || (used & viewBit) != 0u)
            continue;
        m_viewSlots.emplace(viewBit, ViewSlot {camera});
        updateDistanceVisibility();
        return viewBit;
    }
    return 0u;
}

void OgreScene::releaseViewSlot(std::uint32_t viewBit)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (viewBit == 0u || m_viewSlots.erase(viewBit) == 0u)
        return;
    updateDistanceVisibility();
}

bool OgreScene::buildCurrentModel(const ModelMeshDto& mesh, const PreparedUpload& prepared)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    // 点云（PLY / LAS / E57）只有顶点没有索引，不能因为 indices 为空就拒绝上传。
    if (!ready() || mesh.vertices.empty() || mesh.primitives.empty()) {
        return false;
    }
    try {
        const std::uint64_t uploadSerial = nextOgreResourceSerial();
        std::cerr << "VIEW3D_OGRE_STAGE: upload_begin vertices=" << mesh.vertices.size()
                  << " indices=" << mesh.indices.size() << "\n";
        Ogre::Hlms* pbs = m_root->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        if (!pbs) {
            clearCurrentModel();
            return false;
        }
        // 逐顶点颜色只有 Unlit 着色器有 COLOR0 输入；拿不到就整套退回 PBS。
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        // 没有环境反射（IBL/CubemapProbe）时，金属工作流只剩方向光的一点高光，
        // metallic≈1 的材质会渲染成纯黑（DamagedHelmet 的面罩就是这种）。默认把
        // 金属度压到 0，让金属件按自身 baseColor 参与漫反射；
        // VIEW3D_PBS_METAL=1 保留作者设定的金属度。
        const bool keepAuthoredMetalness = qEnvironmentVariableIsSet("VIEW3D_PBS_METAL");
        Ogre::TextureGpuManager* textureManager =
            m_root->getRenderSystem()->getTextureGpuManager();
        // 提交阶段只做去重查找 + 显存拷贝，哈希与通道展开已在工作线程完成。
        const auto acquireTexture = [&](const PreparedTexture& item) -> Ogre::TextureGpu* {
            if (!item.valid)
                return nullptr;
            const auto cached = m_textureCache.find(item.cacheKey);
            if (cached != m_textureCache.end()) {
                ++cached->second.refs;
                m_modelTextures.push_back(cached->second.texture);
                return cached->second.texture;
            }
            const Ogre::PixelFormatGpu format =
                item.srgb ? Ogre::PFG_RGBA8_UNORM_SRGB : Ogre::PFG_RGBA8_UNORM;
            const Ogre::String textureName =
                "View3DTexture" + Ogre::StringConverter::toString(uploadSerial) + "_" +
                std::to_string(item.sourceIndex) + "_" +
                std::to_string(static_cast<unsigned>(item.channel)) + "_" +
                std::to_string(static_cast<unsigned>(item.strengthKey)) + "_" +
                (item.srgb ? "srgb" : "linear");
            Ogre::TextureGpu* texture = textureManager->createTexture(
                textureName, Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture,
                Ogre::TextureTypes::Type2D);
            m_modelTextures.push_back(texture);
            texture->setResolution(
                static_cast<Ogre::uint32>(item.width),
                static_cast<Ogre::uint32>(item.height));
            texture->setPixelFormat(format);
            texture->scheduleTransitionTo(Ogre::GpuResidency::Resident);
            Ogre::StagingTexture* staging = nullptr;
            bool stagingMapped = false;
            try {
                staging = textureManager->getStagingTexture(
                    static_cast<Ogre::uint32>(item.width),
                    static_cast<Ogre::uint32>(item.height), 1u, 1u, format);
                staging->startMapRegion();
                stagingMapped = true;
                Ogre::TextureBox box = staging->mapRegion(
                    static_cast<Ogre::uint32>(item.width),
                    static_cast<Ogre::uint32>(item.height), 1u, 1u, format);
                box.copyFrom(
                    item.rgba.data(), static_cast<Ogre::uint32>(item.width),
                    static_cast<Ogre::uint32>(item.height),
                    static_cast<Ogre::uint32>(item.width * 4));
                staging->stopMapRegion();
                stagingMapped = false;
                staging->upload(box, texture, 0u, 0u, 0u);
                textureManager->removeStagingTexture(staging);
                staging = nullptr;
            } catch (...) {
                if (staging) {
                    if (stagingMapped) {
                        try {
                            staging->stopMapRegion();
                        } catch (...) {
                        }
                    }
                    textureManager->removeStagingTexture(staging);
                }
                throw;
            }
            ++m_uploadedTextureCount;
            m_textureCache.emplace(item.cacheKey, TextureCacheEntry { texture, 1u });
            m_textureKeys.emplace(texture, item.cacheKey);
            return texture;
        };
        const auto findPrepared = [&](std::int32_t sourceIndex, std::uint8_t channel,
                                      std::uint16_t strengthKey, bool srgb)
            -> const PreparedTexture* {
            for (const PreparedTexture& item : prepared.textures) {
                if (item.sourceIndex == sourceIndex && item.channel == channel &&
                    item.strengthKey == strengthKey && item.srgb == srgb)
                    return &item;
            }
            return nullptr;
        };
        std::vector<Ogre::TextureGpu*> sourceSrgbTextures(mesh.textures.size(), nullptr);
        for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
            const PreparedTexture* item = findPrepared(static_cast<std::int32_t>(i), 4u, 1000u,
                                                       true);
            if (item)
                sourceSrgbTextures[i] = acquireTexture(*item);
        }
        using SemanticTextureKey = std::tuple<std::int32_t, std::uint8_t, std::uint16_t>;
        std::map<SemanticTextureKey, Ogre::TextureGpu*> linearTextureCache;
        const auto semanticTexture = [&](std::int32_t sourceIndex, std::uint8_t channel,
                                          float strength) -> Ogre::TextureGpu* {
            if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >= mesh.textures.size())
                return nullptr;
            const std::uint16_t strengthKey = static_cast<std::uint16_t>(
                qBound(0, qRound(static_cast<double>(strength) * 1000.0), 1000));
            const SemanticTextureKey key {sourceIndex, channel, strengthKey};
            const auto found = linearTextureCache.find(key);
            if (found != linearTextureCache.end()) return found->second;
            const PreparedTexture* item = findPrepared(sourceIndex, channel, strengthKey, false);
            Ogre::TextureGpu* texture = item ? acquireTexture(*item) : nullptr;
            linearTextureCache.emplace(key, texture);
            if (channel < 4u) ++m_modelDerivedPbrTextureCount;
            return texture;
        };
        const auto derivedSample = [&](std::int32_t sourceIndex, std::uint8_t channel,
                                       float strength) -> std::uint8_t {
            if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >= mesh.textures.size() ||
                channel >= 4u)
                return 0;
            const ModelTextureDto& source = mesh.textures[static_cast<std::size_t>(sourceIndex)];
            if (source.rgba8.size() < 4u) return 0;
            const int sourceValue = std::to_integer<int>(source.rgba8[channel]);
            const int strengthKey = qBound(
                0, qRound(static_cast<double>(strength) * 1000.0), 1000);
            const int value = channel == 0u && strengthKey < 1000
                ? qBound(0, qRound(255.0 + (strengthKey / 1000.0) *
                                             (sourceValue - 255.0)), 255)
                : sourceValue;
            return static_cast<std::uint8_t>(value);
        };
        const auto textureColourSpace = [](const Ogre::TextureGpu* texture) {
            if (!texture) return PbrTextureColourSpace::None;
            return texture->getPixelFormat() == Ogre::PFG_RGBA8_UNORM_SRGB
                ? PbrTextureColourSpace::Srgb : PbrTextureColourSpace::Linear;
        };
        m_modelNode = m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
                          ->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        m_modelSceneNodes.resize(mesh.nodes.size(), nullptr);
        std::vector<Ogre::ManualObject*> primitiveObjects(mesh.primitives.size(), nullptr);
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            const ModelNodeDto& node = mesh.nodes[i];
            Ogre::SceneNode* parent = m_modelNode;
            if (node.parentIndex >= 0) {
                const std::size_t parentIndex = static_cast<std::size_t>(node.parentIndex);
                if (parentIndex >= i || !m_modelSceneNodes[parentIndex]) {
                    clearCurrentModel();
                    return false;
                }
                parent = m_modelSceneNodes[parentIndex];
            }
            Ogre::SceneNode* sceneNode = parent->createChildSceneNode(Ogre::SCENE_DYNAMIC);
            Ogre::Matrix4 local;
            for (std::size_t column = 0; column < 4; ++column)
                for (std::size_t row = 0; row < 4; ++row)
                    local[static_cast<size_t>(row)][static_cast<size_t>(column)] =
                        static_cast<Ogre::Real>(node.localTransform[column * 4 + row]);
            Ogre::Vector3 position;
            Ogre::Vector3 scale;
            Ogre::Quaternion orientation;
            local.decomposition(position, scale, orientation);
            sceneNode->setPosition(position);
            sceneNode->setScale(scale);
            sceneNode->setOrientation(orientation);
            m_modelSceneNodes[i] = sceneNode;
            if (node.primitiveCount > 0) {
                if (node.firstPrimitive > mesh.primitives.size() ||
                    node.primitiveCount > mesh.primitives.size() - node.firstPrimitive) {
                    clearCurrentModel();
                    return false;
                }
                Ogre::ManualObject* object = m_sceneManager->createManualObject();
                object->setCastShadows(true);
                m_modelObjects.push_back(object);
                m_modelObjectNodeIndices.push_back(static_cast<std::uint32_t>(i));
                sceneNode->attachObject(object);
                for (std::uint32_t primitiveIndex = node.firstPrimitive;
                     primitiveIndex < node.firstPrimitive + node.primitiveCount; ++primitiveIndex)
                    primitiveObjects[primitiveIndex] = object;
            }
        }
        if (mesh.nodes.empty()) {
            Ogre::ManualObject* object = m_sceneManager->createManualObject();
            object->setCastShadows(true);
            m_modelObjects.push_back(object);
            m_modelObjectNodeIndices.push_back(0);
            m_modelNode->attachObject(object);
            std::fill(primitiveObjects.begin(), primitiveObjects.end(), object);
        }
        std::size_t unlitPrimitiveCount = 0;
        // 本次上传作用域内的「外观签名 → 共享块下标」表。
        std::map<std::string, std::uint32_t> variantsByKey;
        for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
            const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
            Ogre::ManualObject* object = primitiveObjects[primitiveIndex];
            if (!object) {
                clearCurrentModel();
                return false;
            }
            if (primitive.firstVertex + primitive.vertexCount > mesh.vertices.size() ||
                primitive.firstIndex + primitive.indexCount > mesh.indices.size()) {
                clearCurrentModel();
                return false;
            }
            Ogre::HlmsMacroblock macroblock;
            Ogre::HlmsBlendblock blendblock;
            if (primitive.doubleSided) macroblock.mCullMode = Ogre::CULL_NONE;
            if (primitive.alphaBlend) {
                macroblock.mDepthWrite = false;
                blendblock.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
            }
            // DasViewer（OSG 默认状态集）把 ColorArray 直接乘进最终颜色，不参与光照。
            // PBS 着色器没有 COLOR0 输入（部署的 Hlms/Pbs 里搜不到 hlms_colour），走 PBS
            // 会把逐顶点颜色整条丢掉——只带顶点色的倾斜摄影成果因此看着像白模。
            // 无贴图但有顶点色的基元改走 Unlit + setUseColour(true)，与要素高亮覆盖物
            // 同一套机制。代价：hlms_colour 打开时着色器不看 material.diffuse，
            // 图层不透明度与亮度/对比度对这类基元不起作用（各处按 nullptr 跳过）。
            const bool textureFree = primitive.baseColorTextureIndex < 0 &&
                primitive.metallicRoughnessTextureIndex < 0 &&
                primitive.normalTextureIndex < 0 && primitive.occlusionTextureIndex < 0 &&
                primitive.emissiveTextureIndex < 0;
            const bool vertexColourOnly = unlit && textureFree &&
                primitive.firstVertex < mesh.vertices.size() &&
                mesh.vertices[primitive.firstVertex].hasColor &&
                !qEnvironmentVariableIsSet("VIEW3D_VERTEX_COLOR_PBS");
            Ogre::HlmsPbsDatablock* datablock = nullptr;
            Ogre::HlmsUnlitDatablock* unlitDatablock = nullptr;
            Ogre::String datablockName;
            std::uint32_t variant = 0;
            bool fresh = false;
            // 外观签名相同就共用同一块：一块名字对应一个签名，销毁/遍历/绑定都只看
            // 去重后的表，不再随基元数线性增长。
            const std::string signature = materialSignature(primitive, mesh.textures,
                                                            vertexColourOnly);
            const auto reused = variantsByKey.find(signature);
            if (reused != variantsByKey.end()) {
                variant = reused->second;
                datablockName = m_modelDatablockNames[variant];
                datablock = m_modelDatablocks[variant];
                unlitDatablock = m_modelUnlitDatablocks[variant];
            } else {
                variant = static_cast<std::uint32_t>(m_modelDatablockNames.size());
                datablockName = "View3DMaterial" +
                    Ogre::StringConverter::toString(uploadSerial) + "_" +
                    Ogre::StringConverter::toString(variant);
                variantsByKey.emplace(signature, variant);
                fresh = true;
            }
            if (vertexColourOnly) {
                if (fresh) {
                    unlitDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
                        unlit->createDatablock(datablockName, datablockName, macroblock,
                                               blendblock, Ogre::HlmsParamVec()));
                    ++unlitPrimitiveCount;
                }
                unlitDatablock->setUseColour(true);
                unlitDatablock->setColour(Ogre::ColourValue(
                    static_cast<Ogre::Real>(primitive.baseColorR),
                    static_cast<Ogre::Real>(primitive.baseColorG),
                    static_cast<Ogre::Real>(primitive.baseColorB),
                    static_cast<Ogre::Real>(primitive.baseColorA)));
            } else {
                if (fresh) {
                    datablock = static_cast<Ogre::HlmsPbsDatablock*>(pbs->createDatablock(
                        datablockName, datablockName, macroblock, blendblock,
                        Ogre::HlmsParamVec()));
                }
                datablock->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
                // 有环境反射时保留作者设定的金属度；没有时金属只剩方向光高光，
                // metallic≈1 会渲染成纯黑，压到 0 让它按 baseColor 参与漫反射。
                const bool reflectionReady = m_pbsReflection && m_pbsReflection->ready();
                datablock->setMetalness(keepAuthoredMetalness || reflectionReady
                                            ? static_cast<float>(primitive.metallicFactor)
                                            : 0.0f);
                datablock->setRoughness(static_cast<float>(primitive.roughnessFactor));
                if (reflectionReady)
                    m_pbsReflection->bindDatablock(datablock);
                datablock->setEmissive(Ogre::Vector3(
                    static_cast<Ogre::Real>(primitive.emissiveR),
                    static_cast<Ogre::Real>(primitive.emissiveG),
                    static_cast<Ogre::Real>(primitive.emissiveB)));
                datablock->setDiffuse(Ogre::Vector3(
                    static_cast<Ogre::Real>(primitive.baseColorR),
                    static_cast<Ogre::Real>(primitive.baseColorG),
                    static_cast<Ogre::Real>(primitive.baseColorB)));
                datablock->setTransparency(
                    static_cast<float>(primitive.baseColorA),
                    primitive.alphaBlend ? Ogre::HlmsPbsDatablock::Fade
                                         : Ogre::HlmsPbsDatablock::None,
                    primitive.alphaBlend, true);
                if (primitive.doubleSided)
                    datablock->setTwoSidedLighting(true, false);
            }
            const auto addressMode = [](std::int32_t mode) {
                if (mode == 33071) return Ogre::TAM_CLAMP;
                if (mode == 33648) return Ogre::TAM_MIRROR;
                return Ogre::TAM_WRAP;
            };
            const auto samplerForTexture = [&](const ModelTextureDto& textureDto) {
                Ogre::HlmsSamplerblock sampler;
                sampler.mU = addressMode(textureDto.wrapS);
                sampler.mV = addressMode(textureDto.wrapT);
                sampler.mMagFilter =
                    textureDto.magFilter == 9728 ? Ogre::FO_POINT : Ogre::FO_LINEAR;
                sampler.mMinFilter =
                    (textureDto.minFilter == 9728 || textureDto.minFilter == 9984 ||
                     textureDto.minFilter == 9986)
                        ? Ogre::FO_POINT
                        : Ogre::FO_LINEAR;
                sampler.mMipFilter =
                    (textureDto.minFilter == 9728 || textureDto.minFilter == 9729)
                        ? Ogre::FO_NONE
                        : ((textureDto.minFilter == 9984 || textureDto.minFilter == 9985)
                               ? Ogre::FO_POINT
                               : Ogre::FO_LINEAR);
                return sampler;
            };
            PbrMaterialState pbrState;
            pbrState.metallic = static_cast<float>(primitive.metallicFactor);
            pbrState.roughness = static_cast<float>(primitive.roughnessFactor);
            pbrState.emissive = {
                static_cast<float>(primitive.emissiveR),
                static_cast<float>(primitive.emissiveG),
                static_cast<float>(primitive.emissiveB)};
            pbrState.normalScale = static_cast<float>(primitive.normalScale);
            pbrState.tangentSource = primitive.tangentSource == ModelTangentSource::Explicit
                ? PbrTangentSource::Explicit
                : primitive.tangentSource == ModelTangentSource::Generated
                    ? PbrTangentSource::Generated
                    : PbrTangentSource::None;
            if (primitive.baseColorTextureIndex >= 0 &&
                static_cast<std::size_t>(primitive.baseColorTextureIndex) <
                    sourceSrgbTextures.size() &&
                sourceSrgbTextures[primitive.baseColorTextureIndex]) {
                const ModelTextureDto& textureDto =
                    mesh.textures[primitive.baseColorTextureIndex];
                Ogre::HlmsSamplerblock sampler = samplerForTexture(textureDto);
                datablock->setTexture(
                    Ogre::PBSM_DIFFUSE, sourceSrgbTextures[primitive.baseColorTextureIndex],
                    &sampler);
                datablock->setTextureUvSource(
                    Ogre::PBSM_DIFFUSE,
                    static_cast<Ogre::uint8>(qBound(0, primitive.baseColorTexCoord, 1)));
                pbrState.diffuseTexture = true;
                pbrState.diffuseUv =
                    static_cast<std::uint8_t>(qBound(0, primitive.baseColorTexCoord, 1));
                pbrState.diffuseColourSpace = textureColourSpace(
                    sourceSrgbTextures[primitive.baseColorTextureIndex]);
            }
            if (primitive.metallicRoughnessTextureIndex >= 0 &&
                static_cast<std::size_t>(primitive.metallicRoughnessTextureIndex) <
                    mesh.textures.size()) {
                const ModelTextureDto& textureDto =
                    mesh.textures[primitive.metallicRoughnessTextureIndex];
                Ogre::HlmsSamplerblock sampler = samplerForTexture(textureDto);
                Ogre::TextureGpu* metallicTexture = semanticTexture(
                    primitive.metallicRoughnessTextureIndex, 2u, 1.0f);
                Ogre::TextureGpu* roughnessTexture = semanticTexture(
                    primitive.metallicRoughnessTextureIndex, 1u, 1.0f);
                if (metallicTexture) {
                    datablock->setTexture(Ogre::PBSM_METALLIC, metallicTexture, &sampler);
                    datablock->setTextureUvSource(
                        Ogre::PBSM_METALLIC,
                        static_cast<Ogre::uint8>(qBound(0, primitive.metallicRoughnessTexCoord, 1)));
                    pbrState.metallicTexture = true;
                    pbrState.metallicUv = static_cast<std::uint8_t>(
                        qBound(0, primitive.metallicRoughnessTexCoord, 1));
                    pbrState.metallicDerivedSample = derivedSample(
                        primitive.metallicRoughnessTextureIndex, 2u, 1.0f);
                    pbrState.metallicColourSpace = textureColourSpace(metallicTexture);
                }
                if (roughnessTexture) {
                    datablock->setTexture(Ogre::PBSM_ROUGHNESS, roughnessTexture, &sampler);
                    datablock->setTextureUvSource(
                        Ogre::PBSM_ROUGHNESS,
                        static_cast<Ogre::uint8>(qBound(0, primitive.metallicRoughnessTexCoord, 1)));
                    pbrState.roughnessTexture = true;
                    pbrState.roughnessUv = static_cast<std::uint8_t>(
                        qBound(0, primitive.metallicRoughnessTexCoord, 1));
                    pbrState.roughnessDerivedSample = derivedSample(
                        primitive.metallicRoughnessTextureIndex, 1u, 1.0f);
                    pbrState.roughnessColourSpace = textureColourSpace(roughnessTexture);
                }
            }
            if (primitive.normalMappingEnabled && primitive.normalTextureIndex >= 0 &&
                static_cast<std::size_t>(primitive.normalTextureIndex) < mesh.textures.size()) {
                const ModelTextureDto& textureDto = mesh.textures[primitive.normalTextureIndex];
                Ogre::HlmsSamplerblock sampler = samplerForTexture(textureDto);
                Ogre::TextureGpu* normalTexture =
                    semanticTexture(primitive.normalTextureIndex, 4u, 1.0f);
                if (normalTexture) {
                    datablock->setTexture(Ogre::PBSM_NORMAL, normalTexture, &sampler);
                    datablock->setTextureUvSource(
                        Ogre::PBSM_NORMAL,
                        static_cast<Ogre::uint8>(qBound(0, primitive.normalTexCoord, 1)));
                    datablock->setNormalMapWeight(static_cast<Ogre::Real>(primitive.normalScale));
                    pbrState.normalTexture = true;
                    pbrState.normalUv =
                        static_cast<std::uint8_t>(qBound(0, primitive.normalTexCoord, 1));
                    pbrState.normalColourSpace = textureColourSpace(normalTexture);
                }
            }
            if (primitive.occlusionTextureIndex >= 0 &&
                static_cast<std::size_t>(primitive.occlusionTextureIndex) < mesh.textures.size()) {
                const ModelTextureDto& textureDto = mesh.textures[primitive.occlusionTextureIndex];
                Ogre::HlmsSamplerblock sampler = samplerForTexture(textureDto);
                Ogre::TextureGpu* occlusionTexture = semanticTexture(
                    primitive.occlusionTextureIndex, 0u,
                    static_cast<float>(primitive.occlusionStrength));
                if (occlusionTexture) {
                    datablock->setTexture(Ogre::PBSM_DETAIL0, occlusionTexture, &sampler);
                    datablock->setTextureUvSource(
                        Ogre::PBSM_DETAIL0,
                        static_cast<Ogre::uint8>(qBound(0, primitive.occlusionTexCoord, 1)));
                    datablock->setDetailMapBlendMode(0u, Ogre::PBSM_BLEND_MULTIPLY);
                    pbrState.occlusionTexture = true;
                    pbrState.occlusionUv =
                        static_cast<std::uint8_t>(qBound(0, primitive.occlusionTexCoord, 1));
                    pbrState.occlusionDerivedSample = derivedSample(
                        primitive.occlusionTextureIndex, 0u,
                        static_cast<float>(primitive.occlusionStrength));
                    pbrState.occlusionColourSpace = textureColourSpace(occlusionTexture);
                    pbrState.aoApproximation = true;
                }
            }
            if (primitive.emissiveTextureIndex >= 0 &&
                static_cast<std::size_t>(primitive.emissiveTextureIndex) <
                    sourceSrgbTextures.size() &&
                sourceSrgbTextures[primitive.emissiveTextureIndex]) {
                const ModelTextureDto& textureDto = mesh.textures[primitive.emissiveTextureIndex];
                Ogre::HlmsSamplerblock sampler = samplerForTexture(textureDto);
                datablock->setTexture(
                    Ogre::PBSM_EMISSIVE, sourceSrgbTextures[primitive.emissiveTextureIndex],
                    &sampler);
                datablock->setTextureUvSource(
                    Ogre::PBSM_EMISSIVE,
                    static_cast<Ogre::uint8>(qBound(0, primitive.emissiveTexCoord, 1)));
                pbrState.emissiveTexture = true;
                pbrState.emissiveUv =
                    static_cast<std::uint8_t>(qBound(0, primitive.emissiveTexCoord, 1));
                pbrState.emissiveColourSpace = textureColourSpace(
                    sourceSrgbTextures[primitive.emissiveTextureIndex]);
            }
            if (primitive.alphaMask && datablock) {
                datablock->setAlphaTest(Ogre::CMPF_GREATER_EQUAL);
                const double baseAlpha = primitive.baseColorA;
                const float effectiveCutoff = baseAlpha <= 0.0
                    ? 2.0f
                    : static_cast<float>(primitive.alphaCutoff / baseAlpha);
                datablock->setAlphaTestThreshold(effectiveCutoff);
            }
            if (fresh) {
                m_modelDatablockNames.push_back(datablockName);
                m_modelDatablocks.push_back(datablock);
                m_modelUnlitDatablocks.push_back(unlitDatablock);
                m_modelBaseColours.push_back({
                    static_cast<float>(primitive.baseColorR),
                    static_cast<float>(primitive.baseColorG),
                    static_cast<float>(primitive.baseColorB),
                    static_cast<float>(primitive.baseColorA)});
                m_modelAlphaBlended.push_back(primitive.alphaBlend);
                m_modelPbrMaterialStates.push_back(pbrState);
            }
            m_modelPrimitiveDatablocks.push_back(variant);
            object->begin(
                datablockName,
                primitive.topology == ModelPrimitiveTopology::Points
                    ? Ogre::OT_POINT_LIST : Ogre::OT_TRIANGLE_LIST);
            for (std::uint32_t i = 0; i < primitive.vertexCount; ++i) {
                const MeshVertexDto& vertex = mesh.vertices[primitive.firstVertex + i];
                object->position(
                    static_cast<Ogre::Real>(vertex.x), static_cast<Ogre::Real>(vertex.y),
                    static_cast<Ogre::Real>(vertex.z));
                if (vertex.hasNormal) {
                    object->normal(
                        static_cast<Ogre::Real>(vertex.nx), static_cast<Ogre::Real>(vertex.ny),
                        static_cast<Ogre::Real>(vertex.nz));
                }
                if (vertex.hasTexCoord) {
                    object->textureCoord(
                        static_cast<Ogre::Real>(vertex.u), static_cast<Ogre::Real>(vertex.v));
                }
                if (vertex.hasTexCoord1) {
                    object->textureCoord(
                        static_cast<Ogre::Real>(vertex.u1), static_cast<Ogre::Real>(vertex.v1));
                }
                if (primitive.normalMappingEnabled && vertex.hasTangent) {
                    object->tangent(
                        static_cast<Ogre::Real>(vertex.tx),
                        static_cast<Ogre::Real>(vertex.ty),
                        static_cast<Ogre::Real>(vertex.tz));
                }
                // 每个顶点都必须发出 colour 语义，否则 ManualObject 的顶点布局会
                // 前后不一致；没有逐点颜色时退回该 primitive 的 baseColor。
                if (vertex.hasColor) {
                    object->colour(static_cast<Ogre::Real>(vertex.colorR) / 255.0f,
                                   static_cast<Ogre::Real>(vertex.colorG) / 255.0f,
                                   static_cast<Ogre::Real>(vertex.colorB) / 255.0f,
                                   static_cast<Ogre::Real>(vertex.colorA) / 255.0f);
                } else {
                    object->colour(
                        static_cast<Ogre::Real>(primitive.baseColorR),
                        static_cast<Ogre::Real>(primitive.baseColorG),
                        static_cast<Ogre::Real>(primitive.baseColorB),
                        static_cast<Ogre::Real>(primitive.baseColorA));
                }
            }
            if (primitive.topology == ModelPrimitiveTopology::Points) {
                // ManualObject::end() 在 Ogre-Next 3 中要求存在索引缓冲，
                // 点列表必须显式补一组顺序索引。
                for (std::uint32_t i = 0; i < primitive.vertexCount; ++i)
                    object->index(i);
            } else {
                for (std::uint32_t i = 0; i < primitive.indexCount; ++i) {
                    const std::uint32_t globalIndex = mesh.indices[primitive.firstIndex + i];
                    if (globalIndex < primitive.firstVertex ||
                        globalIndex >= primitive.firstVertex + primitive.vertexCount) {
                        clearCurrentModel();
                        return false;
                    }
                    object->index(globalIndex - primitive.firstVertex);
                }
            }
            Ogre::ManualObject::ManualObjectSection* section = object->end();
            // 传 nullptr 会让这段退回默认材质，必须按分支给回真正建的 datablock。
            section->setDatablock(vertexColourOnly
                ? static_cast<Ogre::HlmsDatablock*>(unlitDatablock)
                : static_cast<Ogre::HlmsDatablock*>(datablock));
        }
        std::cerr << "VIEW3D_OGRE_STAGE: gpu_buffers_ready unlit_primitives="
                  << unlitPrimitiveCount << " datablocks=" << m_modelDatablockNames.size()
                  << " primitives=" << mesh.primitives.size() << "\n";

        m_uploadedVertexCount = mesh.vertices.size();
        m_uploadedIndexCount = mesh.indices.size();
        m_uploadedPrimitiveCount = static_cast<std::uint32_t>(mesh.primitives.size());
        m_modelHasBounds = prepared.hasBounds;
        if (prepared.hasBounds) {
            m_modelBoundsMinimum = prepared.boundsMinimum;
            m_modelBoundsMaximum = prepared.boundsMaximum;
        }
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_MODEL_UPLOAD_FAILED: " << error.getFullDescription() << "\n";
        clearCurrentModel();
        return false;
    }
}


OgreScene::ModelResources OgreScene::takeCurrentModel()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ModelResources resources;
    resources.objects = std::move(m_modelObjects);
    resources.objectNodeIndices = std::move(m_modelObjectNodeIndices);
    resources.rootNode = m_modelNode;
    resources.sceneNodes = std::move(m_modelSceneNodes);
    resources.textures = std::move(m_modelTextures);
    resources.datablockNames = std::move(m_modelDatablockNames);
    resources.datablocks = std::move(m_modelDatablocks);
    resources.unlitDatablocks = std::move(m_modelUnlitDatablocks);
    resources.baseColours = std::move(m_modelBaseColours);
    resources.alphaBlended = std::move(m_modelAlphaBlended);
    resources.pbrMaterialStates = std::move(m_modelPbrMaterialStates);
    resources.primitiveDatablocks = std::move(m_modelPrimitiveDatablocks);
    resources.derivedPbrTextureCount = m_modelDerivedPbrTextureCount;
    resources.edgeOverlayObject = m_modelEdgeOverlayObject;
    resources.edgeOverlayDatablock = m_modelEdgeOverlayDatablock;
    resources.edgeOverlayDatablockName = std::move(m_modelEdgeOverlayDatablockName);
    resources.vertexCount = m_uploadedVertexCount;
    resources.indexCount = m_uploadedIndexCount;
    resources.textureCount = m_uploadedTextureCount;
    resources.primitiveCount = m_uploadedPrimitiveCount;
    resources.selectedFirstPrimitive = m_selectedFirstPrimitive;
    resources.selectedPrimitiveCount = m_selectedPrimitiveCount;
    resources.pickMesh = std::move(m_modelPickMesh);
    resources.visible = m_modelVisible;
    resources.hasBounds = m_modelHasBounds;
    resources.boundsMinimum = m_modelBoundsMinimum;
    resources.boundsMaximum = m_modelBoundsMaximum;
    m_modelNode = nullptr;
    m_uploadedVertexCount = 0;
    m_uploadedIndexCount = 0;
    m_uploadedTextureCount = 0;
    m_uploadedPrimitiveCount = 0;
    m_modelDerivedPbrTextureCount = 0;
    m_selectedFirstPrimitive = 0;
    m_selectedPrimitiveCount = 0;
    m_modelPickMesh.reset();
    m_modelEdgeOverlayObject = nullptr;
    m_modelEdgeOverlayDatablock = nullptr;
    m_modelEdgeOverlayDatablockName.clear();
    m_modelVisible = true;
    m_modelHasBounds = false;
    m_modelBoundsMinimum = {};
    m_modelBoundsMaximum = {};
    return resources;
}

void OgreScene::installCurrentModel(ModelResources&& resources)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_modelObjects = std::move(resources.objects);
    m_modelObjectNodeIndices = std::move(resources.objectNodeIndices);
    m_modelNode = resources.rootNode;
    m_modelSceneNodes = std::move(resources.sceneNodes);
    m_modelTextures = std::move(resources.textures);
    m_modelDatablockNames = std::move(resources.datablockNames);
    m_modelDatablocks = std::move(resources.datablocks);
    m_modelUnlitDatablocks = std::move(resources.unlitDatablocks);
    m_modelBaseColours = std::move(resources.baseColours);
    m_modelAlphaBlended = std::move(resources.alphaBlended);
    m_modelPbrMaterialStates = std::move(resources.pbrMaterialStates);
    m_modelPrimitiveDatablocks = std::move(resources.primitiveDatablocks);
    m_modelDerivedPbrTextureCount = resources.derivedPbrTextureCount;
    m_modelEdgeOverlayObject = resources.edgeOverlayObject;
    m_modelEdgeOverlayDatablock = resources.edgeOverlayDatablock;
    m_modelEdgeOverlayDatablockName = std::move(resources.edgeOverlayDatablockName);
    m_uploadedVertexCount = resources.vertexCount;
    m_uploadedIndexCount = resources.indexCount;
    m_uploadedTextureCount = resources.textureCount;
    m_uploadedPrimitiveCount = resources.primitiveCount;
    m_selectedFirstPrimitive = resources.selectedFirstPrimitive;
    m_selectedPrimitiveCount = resources.selectedPrimitiveCount;
    m_modelPickMesh = std::move(resources.pickMesh);
    m_modelVisible = resources.visible;
    m_modelHasBounds = resources.hasBounds;
    m_modelBoundsMinimum = resources.boundsMinimum;
    m_modelBoundsMaximum = resources.boundsMaximum;
}


bool OgreScene::effectiveVisible(const ModelResources& resources) noexcept
{
    // 球面模式下尚未球面化的平面几何不参与渲染：数据插件异步重烘焙完成后
    // 换上新网格再显示，避免平面坐标被当成 ECEF 摆在地心附近。
    if (m_globeMode && resources.sourceMesh && !resources.sourceMesh->hasContentOrigin)
        return false;
    return resources.visible && resources.userVisible;
}

// rootNode->setVisible(cascade=true) 会把子节点标志一并覆盖，所以每次图层
// 可见性变化后都要重新按节点掩码下发；bit 未覆盖到的节点（索引<0 或越界）
// 跟随整块瓦片，不会因为掩码丢失细节。
void OgreScene::applyNodeVisibility(ModelResources& resources)
{
    if (!resources.sourceMesh)
        return;
    const std::vector<ModelNodeDto>& nodes = resources.sourceMesh->nodes;
    const bool layerVisible = effectiveVisible(resources);
    for (std::size_t i = 0; i < resources.sceneNodes.size() && i < nodes.size(); ++i) {
        Ogre::SceneNode* sceneNode = resources.sceneNodes[i];
        if (!sceneNode)
            continue;
        const std::int32_t sourceIndex = nodes[i].sourceNodeIndex;
        const bool nodeVisible = sourceIndex < 0 || sourceIndex >= 64 ||
            ((resources.nodeMask >> sourceIndex) & 1ull) != 0ull;
        sceneNode->setVisible(layerVisible && nodeVisible, false);
    }
}

float OgreScene::effectiveOpacity(const ModelResources& resources) noexcept
{
    return qBound(0.0f, resources.opacity * resources.groupOpacity, 1.0f);
}

std::array<double, 3> OgreScene::effectiveOffset(const ModelResources& resources) noexcept
{
    // 平移在旋转/缩放之后叠加：groupPlacement 是地理摆放，本身就是场景帧里的坐标。
    // 球面模式用 contentOrigin − renderOrigin 顶替它，双精度相减后才落 float。
    const std::array<double, 3>& geo =
        resources.globeAnchored ? resources.globeBasePosition : resources.groupPlacement;
    return {resources.offset[0] + resources.groupOffset[0] + geo[0],
            resources.offset[1] + resources.groupOffset[1] + geo[1],
            resources.offset[2] + resources.groupOffset[2] + geo[2]};
}

void OgreScene::applyLayerPosition(ModelResources& resources)
{
    if (!resources.rootNode)
        return;
    const std::array<double, 3> offset = effectiveOffset(resources);
    resources.rootNode->setPosition(static_cast<Ogre::Real>(offset[0]),
                                    static_cast<Ogre::Real>(offset[1]),
                                    static_cast<Ogre::Real>(offset[2]));
    // 显示投影切换后瓦片组要整体转到网格北：绕 +Y 右手旋转 + 水平各向同性缩放，
    // 与 placeLocalBounds/worldToLocalPoint 用的是同一个约定。
    // 球面锚定的图层不吃组旋转/缩放：内容变换里已经含 ECEF 姿态。
    const double yawDegrees = resources.placementYawDegrees();
    if (std::abs(yawDegrees) > 1e-12) {
        resources.rootNode->setOrientation(
            Ogre::Quaternion(Ogre::Radian(Ogre::Degree(yawDegrees)),
                             Ogre::Vector3::UNIT_Y));
    } else {
        resources.rootNode->setOrientation(Ogre::Quaternion::IDENTITY);
    }
    const float scale = static_cast<float>(resources.placementScale());
    resources.rootNode->setScale(Ogre::Vector3(scale, 1.0f, scale));
}

void OgreScene::placeLocalBounds(const std::array<double, 3>& localMinimum,
                                 const std::array<double, 3>& localMaximum,
                                 const ModelResources& resources, RenderBounds* out)
{
    if (!out)
        return;
    *out = {};
    const double yaw = resources.placementYawDegrees() * kDegreesToRadians;
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    const double scale = resources.placementScale();
    const std::array<double, 3> offset = effectiveOffset(resources);
    out->minimum.fill(std::numeric_limits<double>::max());
    out->maximum.fill(std::numeric_limits<double>::lowest());
    // 八个角点逐个摆过去再取 AABB：闭式公式在旋转+缩放下容易写错符号。
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<double, 3> local {
            (corner & 1) ? localMaximum[0] : localMinimum[0],
            (corner & 2) ? localMaximum[1] : localMinimum[1],
            (corner & 4) ? localMaximum[2] : localMinimum[2]};
        const std::array<double, 3> world {
            offset[0] + scale * (local[0] * c + local[2] * s),
            offset[1] + local[1],
            offset[2] + scale * (-local[0] * s + local[2] * c)};
        for (int axis = 0; axis < 3; ++axis) {
            out->minimum[axis] = qMin(out->minimum[axis], world[axis]);
            out->maximum[axis] = qMax(out->maximum[axis], world[axis]);
        }
    }
    out->valid = out->minimum[0] <= out->maximum[0] && out->minimum[1] <= out->maximum[1] &&
        out->minimum[2] <= out->maximum[2];
}

std::array<double, 3> OgreScene::worldToLocalPoint(const std::array<double, 3>& world,
                                                   const ModelResources& resources)
{
    const double yaw = resources.placementYawDegrees() * kDegreesToRadians;
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    const double scale = resources.placementScale() != 0.0 ? resources.placementScale()
                                                           : 1.0;
    const std::array<double, 3> offset = effectiveOffset(resources);
    const double east = (world[0] - offset[0]) / scale;
    const double north = (world[2] - offset[2]) / scale;
    return {c * east - s * north, world[1] - offset[1], s * east + c * north};
}

std::array<double, 3> OgreScene::localPointToWorld(const std::array<double, 3>& local,
                                                   const ModelResources& resources)
{
    const double yaw = resources.placementYawDegrees() * kDegreesToRadians;
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    const double scale = resources.placementScale();
    const std::array<double, 3> offset = effectiveOffset(resources);
    return {offset[0] + scale * (local[0] * c + local[2] * s), offset[1] + local[1],
            offset[2] + scale * (-local[0] * s + local[2] * c)};
}

void OgreScene::applyLayerOpacity(ModelResources& resources)
{
    const float opacity = effectiveOpacity(resources);
    const std::vector<bool> selected = selectedVariants(resources);
    for (std::size_t i = 0; i < resources.datablocks.size(); ++i) {
        Ogre::HlmsPbsDatablock* datablock = resources.datablocks[i];
        if (!datablock) continue;
        const bool transparent = opacity < 0.9999f ||
            (i < resources.alphaBlended.size() && resources.alphaBlended[i]);
        Ogre::HlmsMacroblock macroblock = *datablock->getMacroblock();
        macroblock.mDepthWrite = !transparent;
        datablock->setMacroblock(macroblock);
        const auto& base = resources.baseColours[i];
        datablock->setTransparency(
            base[3] * opacity,
            transparent ? Ogre::HlmsPbsDatablock::Fade : Ogre::HlmsPbsDatablock::None,
            i < resources.alphaBlended.size() && resources.alphaBlended[i], true);
        datablock->setDiffuse(layerDiffuse(resources, i, i < selected.size() && selected[i]));
    }
}

bool OgreScene::setModelLayerOpacity(const QString& layerKey, float opacity)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end()) return false;
    ModelResources& resources = it->second;
    resources.opacity = qBound(0.0f, opacity, 1.0f);
    applyLayerOpacity(resources);
    return true;
}

Ogre::Vector3 OgreScene::layerDiffuse(const ModelResources& resources, std::size_t variant,
                                      bool selected) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (selected)
        return Ogre::Vector3(1.0f, 0.72f, 0.08f);
    if (variant >= resources.baseColours.size())
        return Ogre::Vector3(1.0f, 1.0f, 1.0f);
    const auto& base = resources.baseColours[variant];
    const auto adjust = [&resources](float value) {
        const float contrasted = 0.5f + (value - 0.5f) * resources.contrast;
        return qBound(0.0f, contrasted + resources.brightness, 2.0f);
    };
    return Ogre::Vector3(
        adjust(base[0] * resources.tint[0]),
        adjust(base[1] * resources.tint[1]),
        adjust(base[2] * resources.tint[2]));
}

// 选中范围落在哪些共享块上。块是按材质去重的，所以同材质的基元会一起被着色——
// 逐基元高亮需要换成 section 级替换，目前这条路径没有调用方在用。
std::vector<bool> OgreScene::selectedVariants(const ModelResources& resources) const
{
    std::vector<bool> selected(resources.datablocks.size(), false);
    if (resources.selectedPrimitiveCount == 0 || resources.primitiveDatablocks.empty())
        return selected;
    const std::size_t first = resources.selectedFirstPrimitive;
    const std::size_t end = (std::min)(
        resources.primitiveDatablocks.size(),
        first + resources.selectedPrimitiveCount);
    for (std::size_t p = (std::min)(first, resources.primitiveDatablocks.size()); p < end; ++p) {
        const std::uint32_t variant = resources.primitiveDatablocks[p];
        if (variant < selected.size())
            selected[variant] = true;
    }
    return selected;
}

bool OgreScene::setModelLayerStyle(const QString& layerKey,
                                            const std::array<float, 3>& tint,
                                            bool edgeLinesVisible, float brightness,
                                            float contrast)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    for (const float component : tint) {
        if (!std::isfinite(component) || component < 0.0f || component > 2.0f)
            return false;
    }
    if (!std::isfinite(brightness) || brightness < -1.0f || brightness > 1.0f ||
        !std::isfinite(contrast) || contrast < 0.5f || contrast > 2.0f)
        return false;
    ModelResources& resources = it->second;
    resources.tint = tint;
    resources.edgeLinesVisible = edgeLinesVisible;
    resources.brightness = brightness;
    resources.contrast = contrast;
    const std::vector<bool> selected = selectedVariants(resources);
    for (std::size_t i = 0; i < resources.datablocks.size(); ++i) {
        Ogre::HlmsPbsDatablock* datablock = resources.datablocks[i];
        if (!datablock) continue;
        datablock->setDiffuse(layerDiffuse(resources, i, i < selected.size() && selected[i]));
    }
    if (resources.edgeOverlayObject)
        resources.edgeOverlayObject->setVisible(edgeLinesVisible);
    return true;
}

bool OgreScene::setModelLayerOffset(const QString& layerKey,
                                             const std::array<double, 3>& offset)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end()) return false;
    for (const double component : offset) {
        if (!qIsFinite(component)) return false;
    }
    it->second.offset = offset;
    applyLayerPosition(it->second);
    return true;
}

bool OgreScene::modelLayerOffset(const QString& layerKey,
                                 std::array<double, 3>* offset) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!offset)
        return false;
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return false;
    *offset = it->second.offset;
    if (it->second.rootNode) {
        const Ogre::Vector3 position = it->second.rootNode->getPosition();
        *offset = {position.x, position.y, position.z};
    }
    return true;
}

bool OgreScene::setModelLayerDistanceLimits(const QString& layerKey,
                                                     double minimumDistance,
                                                     double maximumDistance)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end() || !qIsFinite(minimumDistance) ||
        !qIsFinite(maximumDistance) || minimumDistance < 0.0 || maximumDistance < 0.0)
        return false;
    if (maximumDistance > 0.0 && maximumDistance < minimumDistance)
        return false;
    ModelResources& resources = it->second;
    resources.minimumDistance = minimumDistance;
    resources.maximumDistance = maximumDistance;
    const bool wasDistanceLimited = resources.distanceLimited;
    resources.distanceLimited =
        minimumDistance > 0.0 || maximumDistance > 0.0;
    if (wasDistanceLimited != resources.distanceLimited) {
        if (resources.distanceLimited)
            ++m_distanceLimitedCount;
        else if (m_distanceLimitedCount > 0)
            --m_distanceLimitedCount;
    }
    if (resources.distanceLimited)
        updateDistanceVisibility();
    else
        applyLayerViewMask(resources, allViewsMask());
    return true;
}


float OgreScene::modelLayerOpacity(const QString& layerKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    return it == m_layers.end() ? -1.0f : it->second.opacity;
}

bool OgreScene::selectModelLayerRange(const QString& layerKey,
                                               std::uint32_t firstPrimitive,
                                               std::uint32_t primitiveCount)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end()) return false;
    ModelResources& resources = it->second;
    const std::uint32_t primitiveCountTotal = resources.pending && resources.sourceMesh
        ? static_cast<std::uint32_t>(resources.sourceMesh->primitives.size())
        : resources.primitiveCount;
    if (firstPrimitive > primitiveCountTotal ||
        primitiveCount > primitiveCountTotal - firstPrimitive) return false;
    resources.selectedFirstPrimitive = firstPrimitive;
    resources.selectedPrimitiveCount = primitiveCount;
    const std::vector<bool> selected = selectedVariants(resources);
    for (std::size_t i = 0; i < resources.datablocks.size(); ++i) {
        Ogre::HlmsPbsDatablock* datablock = resources.datablocks[i];
        if (!datablock) continue;
        datablock->setDiffuse(layerDiffuse(resources, i, i < selected.size() && selected[i]));
    }
    return true;
}


void OgreScene::clearFeatureHighlight(ModelResources& resources)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && resources.featureHighlightObject)
        m_sceneManager->destroyManualObject(resources.featureHighlightObject);
    resources.featureHighlightObject = nullptr;
    if (m_root && resources.featureHighlightDatablock) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit && !resources.featureHighlightDatablockName.empty())
            unlit->destroyDatablock(Ogre::IdString(resources.featureHighlightDatablockName));
    }
    resources.featureHighlightDatablock = nullptr;
    resources.featureHighlightDatablockName.clear();
    resources.featureHighlightTriangleCount = 0;
}

void OgreScene::clearFeatureHighlights()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        clearFeatureHighlight(resources);
    }
}

std::uint32_t OgreScene::highlightedFeatureTriangleCount() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    std::uint32_t count = 0;
    for (const auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        count += resources.featureHighlightTriangleCount;
    }
    return count;
}

bool OgreScene::highlightModelFeature(const QString& layerKey,
                                               std::int32_t featureTableIndex,
                                               std::int64_t featureId)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearFeatureHighlights();
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end() || !it->second.pickMesh || !it->second.rootNode ||
        !m_sceneManager || !m_root) return false;
    ModelResources& resources = it->second;
    const ModelMeshDto& mesh = *resources.pickMesh;
    struct HighlightTriangle {
        std::uint32_t primitiveIndex;
        std::uint32_t triangleIndex;
        std::uint32_t nodeIndex;
    };
    std::vector<HighlightTriangle> triangles;
    for (std::uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size();
         ++primitiveIndex) {
        const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
        if (primitive.featureTableIndex != featureTableIndex) continue;
        std::uint32_t ownerNodeIndex = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t nodeIndex = 0; nodeIndex < mesh.nodes.size(); ++nodeIndex) {
            const ModelNodeDto& node = mesh.nodes[nodeIndex];
            if (primitiveIndex >= node.firstPrimitive &&
                primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                ownerNodeIndex = nodeIndex;
                break;
            }
        }
        for (const std::uint32_t triangleIndex :
             mesh.featureTriangles(primitiveIndex, featureId))
            triangles.push_back({primitiveIndex, triangleIndex, ownerNodeIndex});
    }
    if (triangles.empty()) return false;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit) return false;
        const Ogre::String datablockName = "View3DFeatureHighlight" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = false;
        macroblock.mDepthBiasConstant = m_featureHighlightStyle.depthBias;
        macroblock.mDepthBiasSlopeScale = m_featureHighlightStyle.depthBias;
        Ogre::HlmsBlendblock blendblock;
        blendblock.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        auto* datablock = static_cast<Ogre::HlmsUnlitDatablock*>(unlit->createDatablock(
            datablockName, datablockName, macroblock, blendblock, Ogre::HlmsParamVec()));
        resources.featureHighlightDatablock = datablock;
        resources.featureHighlightDatablockName = datablockName;
        datablock->setUseColour(true);
        datablock->setColour(Ogre::ColourValue(
            m_featureHighlightStyle.red, m_featureHighlightStyle.green,
            m_featureHighlightStyle.blue, m_featureHighlightStyle.opacity));
        Ogre::ManualObject* object = m_sceneManager->createManualObject();
        object->setCastShadows(false);
        resources.featureHighlightObject = object;
        resources.rootNode->attachObject(object);
        object->begin(datablockName, Ogre::OT_TRIANGLE_LIST);
        std::uint32_t overlayVertex = 0;
        for (const HighlightTriangle& highlighted : triangles) {
            const ModelPrimitiveDto& primitive = mesh.primitives[highlighted.primitiveIndex];
            const std::uint32_t triangleIndex = highlighted.triangleIndex;
            const std::uint32_t first = primitive.firstIndex + triangleIndex * 3;
            if (first + 2 >= mesh.indices.size()) continue;
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t globalIndex = mesh.indices[first + corner];
                if (globalIndex >= mesh.vertices.size()) continue;
                const MeshVertexDto& vertex = mesh.vertices[globalIndex];
                std::array<double, 3> position {vertex.x, vertex.y, vertex.z};
                if (highlighted.nodeIndex < mesh.nodes.size()) {
                    const auto& m = mesh.nodes[highlighted.nodeIndex].worldTransform;
                    position = {
                        m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
                        m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
                        m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
                }
                object->position(static_cast<Ogre::Real>(position[0]),
                                 static_cast<Ogre::Real>(position[1]),
                                 static_cast<Ogre::Real>(position[2]));
                object->colour(m_featureHighlightStyle.red, m_featureHighlightStyle.green,
                               m_featureHighlightStyle.blue,
                               m_featureHighlightStyle.opacity);
                object->index(overlayVertex++);
            }
        }
        if (overlayVertex == 0) {
            clearFeatureHighlight(resources);
            return false;
        }
        Ogre::ManualObject::ManualObjectSection* section = object->end();
        section->setDatablock(datablock);
        resources.featureHighlightTriangleCount = overlayVertex / 3;
        return true;
    } catch (const Ogre::Exception&) {
        clearFeatureHighlight(resources);
        return false;
    }
}


bool OgreScene::createEdgeOverlay(
    const ModelMeshDto& mesh, Ogre::SceneNode* rootNode, Ogre::ManualObject** object,
    Ogre::HlmsUnlitDatablock** datablock, std::string* datablockName)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!rootNode || !object || !datablock || !datablockName || !m_sceneManager || !m_root)
        return false;
    if (*object) return true;
    bool hasTriangles = false;
    for (const ModelPrimitiveDto& primitive : mesh.primitives)
        hasTriangles = hasTriangles ||
            (primitive.topology != ModelPrimitiveTopology::Points && primitive.indexCount >= 3);
    if (!hasTriangles) return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit) return false;
        const Ogre::String name = "View3DEdgeOverlay" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = false;
        macroblock.mDepthBiasConstant = -1.0f;
        macroblock.mDepthBiasSlopeScale = -1.0f;
        auto* overlayDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(name, name, macroblock, Ogre::HlmsBlendblock(),
                                   Ogre::HlmsParamVec()));
        overlayDatablock->setUseColour(true);
        overlayDatablock->setColour(Ogre::ColourValue(0.92f, 0.95f, 1.0f, 1.0f));
        Ogre::ManualObject* overlay = m_sceneManager->createManualObject();
        overlay->setCastShadows(false);
        rootNode->attachObject(overlay);
        overlay->begin(name, Ogre::OT_LINE_LIST);
        std::uint32_t outputVertex = 0;
        for (std::uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size();
             ++primitiveIndex) {
            const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
            if (primitive.topology == ModelPrimitiveTopology::Points) continue;
            const ModelNodeDto* ownerNode = nullptr;
            for (const ModelNodeDto& node : mesh.nodes) {
                if (primitiveIndex >= node.firstPrimitive &&
                    primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                    ownerNode = &node;
                    break;
                }
            }
            for (std::uint32_t index = 0; index + 2 < primitive.indexCount; index += 3) {
                const std::uint32_t triangle[3] = {
                    mesh.indices[primitive.firstIndex + index],
                    mesh.indices[primitive.firstIndex + index + 1],
                    mesh.indices[primitive.firstIndex + index + 2]};
                const std::uint32_t edges[6] = {
                    triangle[0], triangle[1], triangle[1], triangle[2],
                    triangle[2], triangle[0]};
                for (const std::uint32_t vertexIndex : edges) {
                    if (vertexIndex >= mesh.vertices.size()) continue;
                    const MeshVertexDto& vertex = mesh.vertices[vertexIndex];
                    double x = vertex.x, y = vertex.y, z = vertex.z;
                    if (ownerNode) {
                        const auto& m = ownerNode->worldTransform;
                        x = m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12];
                        y = m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13];
                        z = m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14];
                    }
                    overlay->position(static_cast<Ogre::Real>(x), static_cast<Ogre::Real>(y),
                                      static_cast<Ogre::Real>(z));
                    overlay->colour(0.92f, 0.95f, 1.0f, 1.0f);
                    overlay->index(outputVertex++);
                }
            }
        }
        if (outputVertex == 0) {
            m_sceneManager->destroyManualObject(overlay);
            unlit->destroyDatablock(Ogre::IdString(name));
            return false;
        }
        Ogre::ManualObject::ManualObjectSection* section = overlay->end();
        section->setDatablock(overlayDatablock);
        *object = overlay;
        *datablock = overlayDatablock;
        *datablockName = name;
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    }
}

bool OgreScene::createPointOverlay(
    const ModelMeshDto& mesh, Ogre::SceneNode* rootNode, Ogre::ManualObject** object,
    Ogre::HlmsUnlitDatablock** datablock, std::string* datablockName)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!rootNode || !object || !datablock || !datablockName || !m_sceneManager || !m_root)
        return false;
    if (*object) return true;
    if (mesh.vertices.empty()) return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit) return false;
        const Ogre::String name = "View3DPointOverlay" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = true;
        auto* overlayDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(name, name, macroblock, Ogre::HlmsBlendblock(),
                                   Ogre::HlmsParamVec()));
        overlayDatablock->setUseColour(true);
        overlayDatablock->setColour(Ogre::ColourValue(0.96f, 0.82f, 0.22f, 1.0f));
        Ogre::ManualObject* overlay = m_sceneManager->createManualObject();
        overlay->setCastShadows(false);
        rootNode->attachObject(overlay);
        overlay->begin(name, Ogre::OT_POINT_LIST);
        std::uint32_t outputVertex = 0;
        for (std::uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size();
             ++primitiveIndex) {
            const ModelPrimitiveDto& primitive = mesh.primitives[primitiveIndex];
            const ModelNodeDto* ownerNode = nullptr;
            for (const ModelNodeDto& node : mesh.nodes) {
                if (primitiveIndex >= node.firstPrimitive &&
                    primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                    ownerNode = &node;
                    break;
                }
            }
            for (std::uint32_t vertexOffset = 0; vertexOffset < primitive.vertexCount;
                 ++vertexOffset) {
                const std::uint32_t vertexIndex = primitive.firstVertex + vertexOffset;
                if (vertexIndex >= mesh.vertices.size()) continue;
                const MeshVertexDto& vertex = mesh.vertices[vertexIndex];
                double x = vertex.x, y = vertex.y, z = vertex.z;
                if (ownerNode) {
                    const auto& m = ownerNode->worldTransform;
                    x = m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12];
                    y = m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13];
                    z = m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14];
                }
                overlay->position(static_cast<Ogre::Real>(x), static_cast<Ogre::Real>(y),
                                  static_cast<Ogre::Real>(z));
                overlay->colour(0.96f, 0.82f, 0.22f, 1.0f);
                overlay->index(outputVertex++);
            }
        }
        if (outputVertex == 0) {
            m_sceneManager->destroyManualObject(overlay);
            unlit->destroyDatablock(Ogre::IdString(name));
            return false;
        }
        Ogre::ManualObject::ManualObjectSection* section = overlay->end();
        section->setDatablock(overlayDatablock);
        *object = overlay;
        *datablock = overlayDatablock;
        *datablockName = name;
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    }
}

void OgreScene::clearEdgeOverlay(
    Ogre::ManualObject*& object, Ogre::HlmsUnlitDatablock*& datablock,
    std::string& datablockName)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && object) m_sceneManager->destroyManualObject(object);
    object = nullptr;
    if (m_root && datablock && !datablockName.empty()) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit) unlit->destroyDatablock(Ogre::IdString(datablockName));
    }
    datablock = nullptr;
    datablockName.clear();
}


bool OgreScene::applyDisplayMode(ModelResources& resources)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!resources.pickMesh) return m_displayMode != ViewportDisplayMode::SolidWireframe;
    const Ogre::PolygonMode polygonMode = m_displayMode == ViewportDisplayMode::Wireframe
        ? Ogre::PM_WIREFRAME
        : (m_displayMode == ViewportDisplayMode::Points ? Ogre::PM_POINTS : Ogre::PM_SOLID);
    for (Ogre::HlmsPbsDatablock* datablock : resources.datablocks) {
        if (!datablock) continue;
        Ogre::HlmsMacroblock macroblock = *datablock->getMacroblock();
        macroblock.mPolygonMode = polygonMode;
        datablock->setMacroblock(macroblock);
    }
    // 顶点色通道用的是 Unlit datablock，线框/点模式同样要跟着切。
    for (Ogre::HlmsUnlitDatablock* datablock : resources.unlitDatablocks) {
        if (!datablock) continue;
        Ogre::HlmsMacroblock macroblock = *datablock->getMacroblock();
        macroblock.mPolygonMode = polygonMode;
        datablock->setMacroblock(macroblock);
    }
    for (Ogre::ManualObject* object : resources.objects)
        if (object) object->setVisible(m_displayMode != ViewportDisplayMode::Points);
    if (resources.featureHighlightObject)
        resources.featureHighlightObject->setVisible(
            m_displayMode == ViewportDisplayMode::Solid ||
            m_displayMode == ViewportDisplayMode::SolidWireframe);
    if (m_displayMode == ViewportDisplayMode::SolidWireframe) {
        clearEdgeOverlay(resources.edgeOverlayObject, resources.edgeOverlayDatablock,
                         resources.edgeOverlayDatablockName);
        return createEdgeOverlay(*resources.pickMesh, resources.rootNode,
                                 &resources.edgeOverlayObject,
                                 &resources.edgeOverlayDatablock,
                                 &resources.edgeOverlayDatablockName);
    }
    if (m_displayMode == ViewportDisplayMode::Points) {
        clearEdgeOverlay(resources.edgeOverlayObject, resources.edgeOverlayDatablock,
                         resources.edgeOverlayDatablockName);
        return createPointOverlay(*resources.pickMesh, resources.rootNode,
                                  &resources.edgeOverlayObject,
                                  &resources.edgeOverlayDatablock,
                                  &resources.edgeOverlayDatablockName);
    }
    clearEdgeOverlay(resources.edgeOverlayObject, resources.edgeOverlayDatablock,
                     resources.edgeOverlayDatablockName);
    return true;
}

bool OgreScene::setDisplayMode(ViewportDisplayMode mode)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_displayMode = mode;
    bool applied = true;
    for (auto& [key, resources] : m_layers) {
        static_cast<void>(key);
        if (resources.pending)
            continue;
        applied = applyDisplayMode(resources) && applied;
    }
    return applied;
}


void OgreScene::clearEnvironmentObjects()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager) {
        if (m_gridObject) m_sceneManager->destroyManualObject(m_gridObject);
        if (m_axesObject) m_sceneManager->destroyManualObject(m_axesObject);
    }
    m_gridObject = nullptr;
    m_axesObject = nullptr;
    if (m_root) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit) {
            if (m_gridDatablock && !m_gridDatablockName.empty())
                unlit->destroyDatablock(Ogre::IdString(m_gridDatablockName));
            if (m_axesDatablock && !m_axesDatablockName.empty())
                unlit->destroyDatablock(Ogre::IdString(m_axesDatablockName));
        }
    }
    m_gridDatablock = nullptr;
    m_axesDatablock = nullptr;
    m_gridDatablockName.clear();
    m_axesDatablockName.clear();
}

bool OgreScene::rebuildEnvironmentObjects()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearEnvironmentObjects();
    if (!m_sceneManager || !m_root) return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit) return false;
        Ogre::SceneNode* rootNode = m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        macroblock.mDepthWrite = false;

        m_gridDatablockName = "View3DGrid" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_gridDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_gridDatablockName, m_gridDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        m_gridDatablock->setUseColour(true);
        m_gridDatablock->setColour(Ogre::ColourValue(
            m_environmentStyle.gridRed, m_environmentStyle.gridGreen,
            m_environmentStyle.gridBlue, 1.0f));
        m_gridObject = m_sceneManager->createManualObject();
        m_gridObject->setCastShadows(false);
        rootNode->attachObject(m_gridObject);
        m_gridObject->begin(m_gridDatablockName, Ogre::OT_LINE_LIST);
        const float extent = m_environmentStyle.gridExtent;
        const std::uint32_t divisions = m_environmentStyle.gridDivisions;
        const float step = (extent * 2.0f) / static_cast<float>(divisions);
        std::uint32_t gridVertex = 0;
        for (std::uint32_t i = 0; i <= divisions; ++i) {
            const float coordinate = -extent + static_cast<float>(i) * step;
            const std::array<Ogre::Vector3, 4> vertices {
                Ogre::Vector3(-extent, 0.0f, coordinate),
                Ogre::Vector3(extent, 0.0f, coordinate),
                Ogre::Vector3(coordinate, 0.0f, -extent),
                Ogre::Vector3(coordinate, 0.0f, extent)};
            for (const Ogre::Vector3& position : vertices) {
                m_gridObject->position(position);
                m_gridObject->colour(m_environmentStyle.gridRed,
                                     m_environmentStyle.gridGreen,
                                     m_environmentStyle.gridBlue, 1.0f);
                m_gridObject->index(gridVertex++);
            }
        }
        m_gridObject->end()->setDatablock(m_gridDatablock);
        m_gridObject->setVisible(m_environmentStyle.gridVisible && !m_globeMode);

        m_axesDatablockName = "View3DAxes" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_axesDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_axesDatablockName, m_axesDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        m_axesDatablock->setUseColour(true);
        m_axesDatablock->setColour(Ogre::ColourValue::White);
        m_axesObject = m_sceneManager->createManualObject();
        m_axesObject->setCastShadows(false);
        rootNode->attachObject(m_axesObject);
        m_axesObject->begin(m_axesDatablockName, Ogre::OT_LINE_LIST);
        const float length = m_environmentStyle.axisLength;
        const std::array<Ogre::Vector3, 3> axisEnds {
            Ogre::Vector3(length, 0.0f, 0.0f), Ogre::Vector3(0.0f, length, 0.0f),
            Ogre::Vector3(0.0f, 0.0f, length)};
        const std::array<Ogre::ColourValue, 3> axisColours {
            Ogre::ColourValue(1.0f, 0.15f, 0.15f), Ogre::ColourValue(0.2f, 1.0f, 0.2f),
            Ogre::ColourValue(0.2f, 0.45f, 1.0f)};
        std::uint32_t axisVertex = 0;
        for (std::size_t axis = 0; axis < axisEnds.size(); ++axis) {
            // 二维模式隐藏高程轴（索引 1 = Y）：正俯视时它指向相机，画出来只是一个点。
            if (m_axesPlanar && axis == 1)
                continue;
            m_axesObject->position(Ogre::Vector3::ZERO);
            m_axesObject->colour(axisColours[axis]);
            m_axesObject->index(axisVertex++);
            m_axesObject->position(axisEnds[axis]);
            m_axesObject->colour(axisColours[axis]);
            m_axesObject->index(axisVertex++);
        }
        m_axesObject->end()->setDatablock(m_axesDatablock);
        m_axesObject->setVisible(m_environmentStyle.axesVisible && !m_globeMode);
        return true;
    } catch (const Ogre::Exception&) {
        clearEnvironmentObjects();
        return false;
    }
}

bool OgreScene::setEnvironmentStyle(const ViewportEnvironmentStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (style.gridRed < 0.0f || style.gridRed > 1.0f ||
        style.gridGreen < 0.0f || style.gridGreen > 1.0f ||
        style.gridBlue < 0.0f || style.gridBlue > 1.0f ||
        style.gridExtent < 1.0f || style.gridExtent > 10000000.0f ||
        style.gridDivisions < 2 || style.gridDivisions > 1000 ||
        style.axisLength < 0.1f || style.axisLength > 10000000.0f)
        return false;
    const ViewportEnvironmentStyle previous = m_environmentStyle;
    m_environmentStyle = style;
    if (!m_sceneManager || rebuildEnvironmentObjects()) return true;
    m_environmentStyle = previous;
    rebuildEnvironmentObjects();
    return false;
}


bool OgreScene::setGridVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_environmentStyle.gridVisible = visible;
    if (m_gridObject) m_gridObject->setVisible(visible && !m_globeMode);
    return !m_sceneManager || m_gridObject != nullptr;
}

bool OgreScene::setAxesVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_environmentStyle.axesVisible = visible;
    if (m_axesObject) m_axesObject->setVisible(visible && !m_globeMode);
    return !m_sceneManager || m_axesObject != nullptr;
}

bool OgreScene::setAxesPlanar(bool planar)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_axesPlanar == planar)
        return true;
    m_axesPlanar = planar;
    // 轴几何是一次性生成的线段列表，隐藏/恢复高程轴需要重建。
    return !m_sceneManager || rebuildEnvironmentObjects();
}


bool OgreScene::applyLightingStyle()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_sceneManager) return true;
    try {
        const Ogre::ColourValue ambient = m_lightingStyle.ambientVisible
            ? Ogre::ColourValue(
                  m_lightingStyle.ambientRed * m_lightingStyle.ambientIntensity,
                  m_lightingStyle.ambientGreen * m_lightingStyle.ambientIntensity,
                  m_lightingStyle.ambientBlue * m_lightingStyle.ambientIntensity)
            : Ogre::ColourValue::Black;
        m_sceneManager->setAmbientLight(
            ambient, ambient, Ogre::Vector3::UNIT_Y, 1.0f, 0xffffffffu);
        if (!m_mainLight) return false;
        Ogre::Vector3 direction(
            m_lightingStyle.mainLightDirectionX,
            m_lightingStyle.mainLightDirectionY,
            m_lightingStyle.mainLightDirectionZ);
        if (direction.squaredLength() < 1e-9f)
            direction = Ogre::Vector3::NEGATIVE_UNIT_Y;
        direction.normalise();
        m_mainLight->setType(Ogre::Light::LT_DIRECTIONAL);
        m_mainLight->setDirection(Ogre::Vector3(direction[0], direction[1], direction[2]));
        const Ogre::ColourValue colour(
            m_lightingStyle.mainLightRed,
            m_lightingStyle.mainLightGreen,
            m_lightingStyle.mainLightBlue);
        m_mainLight->setDiffuseColour(colour);
        m_mainLight->setSpecularColour(colour);
        m_mainLight->setPowerScale(m_lightingStyle.mainLightIntensity);
        m_mainLight->setCastShadows(m_lightingStyle.mainLightVisible);
        m_mainLight->setVisible(m_lightingStyle.mainLightVisible);
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    }
}

bool OgreScene::createLightingResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_sceneManager || m_mainLight || m_mainLightNode) return false;
    try {
        m_mainLightNode = m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
                              ->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        m_mainLight = m_sceneManager->createLight();
        m_mainLightNode->attachObject(m_mainLight);
        return applyLightingStyle();
    } catch (const Ogre::Exception&) {
        destroyLightingResources();
        return false;
    }
}

void OgreScene::destroyLightingResources()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && m_mainLightNode) m_mainLightNode->detachAllObjects();
    if (m_sceneManager && m_mainLight) m_sceneManager->destroyLight(m_mainLight);
    m_mainLight = nullptr;
    if (m_sceneManager && m_mainLightNode) m_sceneManager->destroySceneNode(m_mainLightNode);
    m_mainLightNode = nullptr;
}

bool OgreScene::setLightingStyle(const ViewportLightingStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto validColour = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    const auto validIntensity = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 100.0f;
    };
    const double directionLength = std::hypot(
        static_cast<double>(style.mainLightDirectionX),
        static_cast<double>(style.mainLightDirectionY),
        static_cast<double>(style.mainLightDirectionZ));
    if (!validColour(style.ambientRed) || !validColour(style.ambientGreen) ||
        !validColour(style.ambientBlue) || !validIntensity(style.ambientIntensity) ||
        !validColour(style.mainLightRed) || !validColour(style.mainLightGreen) ||
        !validColour(style.mainLightBlue) || !validIntensity(style.mainLightIntensity) ||
        !std::isfinite(style.mainLightDirectionX) ||
        !std::isfinite(style.mainLightDirectionY) ||
        !std::isfinite(style.mainLightDirectionZ) ||
        !std::isfinite(directionLength) || directionLength <= 1e-6)
        return false;
    const ViewportLightingStyle previous = m_lightingStyle;
    m_lightingStyle = style;
    if (!m_sceneManager || applyLightingStyle()) return true;
    m_lightingStyle = previous;
    applyLightingStyle();
    return false;
}

bool OgreScene::setAmbientLightVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ViewportLightingStyle style = m_lightingStyle;
    style.ambientVisible = visible;
    return setLightingStyle(style);
}

bool OgreScene::setMainLightVisible(bool visible)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    ViewportLightingStyle style = m_lightingStyle;
    style.mainLightVisible = visible;
    return setLightingStyle(style);
}



void OgreScene::notifyLayerState(const QString& layerKey, LayerRenderState state)
{
    // 上传可能发生在渲染线程，图层状态回调只能落到 UI 线程执行。
    m_runtime.postToUi([this, alive = m_alive, layerKey, state] {
        if (!alive->load(std::memory_order_acquire))
            return;
        dispatchLayerState(layerKey, state);
    });
}

void OgreScene::dispatchLayerState(const QString& layerKey, LayerRenderState state)
{
    std::function<void(const QString&, LayerRenderState)> callback;
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        callback = m_layerStateCallback;
    }
    if (callback)
        callback(layerKey, state);
}

void OgreScene::releaseTexture(Ogre::TextureGpu* texture)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!texture)
        return;
    const auto keyIt = m_textureKeys.find(texture);
    if (keyIt != m_textureKeys.end()) {
        const auto cacheIt = m_textureCache.find(keyIt->second);
        if (cacheIt != m_textureCache.end()) {
            if (cacheIt->second.refs > 1u) {
                --cacheIt->second.refs;
                return;
            }
            m_textureCache.erase(cacheIt);
        }
        m_textureKeys.erase(keyIt);
    }
    if (m_root) {
        auto* textureManager = m_root->getRenderSystem()->getTextureGpuManager();
        textureManager->destroyTexture(texture);
    }
}

void OgreScene::clearCurrentModel()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearEdgeOverlay(m_modelEdgeOverlayObject, m_modelEdgeOverlayDatablock,
                     m_modelEdgeOverlayDatablockName);
    if (m_sceneManager) {
        for (Ogre::ManualObject* object : m_modelObjects) {
            if (object) m_sceneManager->destroyManualObject(object);
        }
    }
    m_modelObjects.clear();
    m_modelObjectNodeIndices.clear();
    m_uploadedPrimitiveCount = 0;
    m_selectedFirstPrimitive = 0;
    m_selectedPrimitiveCount = 0;
    m_modelVisible = true;
    if (m_root) {
        Ogre::HlmsManager* hlmsManager = m_root->getHlmsManager();
        Ogre::Hlms* pbs = hlmsManager->getHlms(Ogre::HLMS_PBS);
        Ogre::Hlms* unlit = hlmsManager->getHlms(Ogre::HLMS_UNLIT);
        for (std::size_t i = 0; i < m_modelDatablockNames.size(); ++i) {
            const Ogre::IdString name(m_modelDatablockNames[i]);
            // 两条通道分别登记在各自的 Hlms 里，按并行下标销毁。
            if (i < m_modelUnlitDatablocks.size() && m_modelUnlitDatablocks[i]) {
                if (unlit) unlit->destroyDatablock(name);
            } else if (pbs) {
                pbs->destroyDatablock(name);
            }
        }
        for (Ogre::TextureGpu* texture : m_modelTextures)
            releaseTexture(texture);
    }
    if (m_sceneManager) {
        for (auto it = m_modelSceneNodes.rbegin(); it != m_modelSceneNodes.rend(); ++it) {
            if (*it) m_sceneManager->destroySceneNode(*it);
        }
    }
    m_modelSceneNodes.clear();
    if (m_sceneManager && m_modelNode) {
        m_sceneManager->destroySceneNode(m_modelNode);
    }
    m_modelNode = nullptr;
    m_modelDatablockNames.clear();
    m_modelDatablocks.clear();
    m_modelUnlitDatablocks.clear();
    m_modelBaseColours.clear();
    m_modelAlphaBlended.clear();
    m_modelPbrMaterialStates.clear();
    m_modelPrimitiveDatablocks.clear();
    m_modelDerivedPbrTextureCount = 0;
    m_modelTextures.clear();
    m_modelPickMesh.reset();
    m_modelHasBounds = false;
    m_modelBoundsMinimum = {};
    m_modelBoundsMaximum = {};
    m_uploadedVertexCount = 0;
    m_uploadedIndexCount = 0;
    m_uploadedTextureCount = 0;
}


void OgreScene::clearTileDebugOverlay()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (m_sceneManager && m_tileDebugObject)
        m_sceneManager->destroyManualObject(m_tileDebugObject);
    m_tileDebugObject = nullptr;
    if (m_root && m_tileDebugDatablock && !m_tileDebugDatablockName.empty()) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit)
            unlit->destroyDatablock(Ogre::IdString(m_tileDebugDatablockName));
    }
    m_tileDebugDatablock = nullptr;
    m_tileDebugDatablockName.clear();
}

void OgreScene::rebuildTileDebugOverlay()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    clearTileDebugOverlay();
    if (!m_tileDebugEnabled || !m_sceneManager || !m_root)
        return;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit)
            return;
        Ogre::HlmsMacroblock macroblock;
        macroblock.mCullMode = Ogre::CULL_NONE;
        m_tileDebugDatablockName = "View3DTileDebug" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_tileDebugDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_tileDebugDatablockName,
                                   m_tileDebugDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        m_tileDebugDatablock->setUseColour(true);
        m_tileDebugDatablock->setColour(Ogre::ColourValue(0.2f, 1.0f, 0.6f, 1.0f));
        m_tileDebugObject = m_sceneManager->createManualObject();
        m_tileDebugObject->setCastShadows(false);
        m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
            ->attachObject(m_tileDebugObject);
        m_tileDebugObject->begin(m_tileDebugDatablockName, Ogre::OT_LINE_LIST);
        const std::array<Ogre::Vector3, 8> corners = {
            Ogre::Vector3(0, 0, 0), Ogre::Vector3(1, 0, 0),
            Ogre::Vector3(1, 1, 0), Ogre::Vector3(0, 1, 0),
            Ogre::Vector3(0, 0, 1), Ogre::Vector3(1, 0, 1),
            Ogre::Vector3(1, 1, 1), Ogre::Vector3(0, 1, 1)};
        const std::array<std::uint32_t, 24> edges = {
            0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
        std::uint32_t vertexCounter = 0;
        for (const auto& [key, resources] : m_layers) {
            if (!key.startsWith(QStringLiteral("tile:")) || !resources.hasBounds)
                continue;
            const Ogre::Vector3 minimum(
                static_cast<Ogre::Real>(resources.boundsMinimum[0]),
                static_cast<Ogre::Real>(resources.boundsMinimum[1]),
                static_cast<Ogre::Real>(resources.boundsMinimum[2]));
            const Ogre::Vector3 maximum(
                static_cast<Ogre::Real>(resources.boundsMaximum[0]),
                static_cast<Ogre::Real>(resources.boundsMaximum[1]),
                static_cast<Ogre::Real>(resources.boundsMaximum[2]));
            const Ogre::Vector3 size = maximum - minimum;
            for (const Ogre::Vector3& corner : corners) {
                m_tileDebugObject->position(minimum + size * corner);
                m_tileDebugObject->colour(0.2f, 1.0f, 0.6f, 1.0f);
                m_tileDebugObject->index(vertexCounter++);
            }
            for (const std::uint32_t edge : edges)
                m_tileDebugObject->index(vertexCounter - 8 + edge);
        }
        m_tileDebugObject->end()->setDatablock(m_tileDebugDatablock);
    } catch (const Ogre::Exception&) {
        if (m_sceneManager && m_tileDebugObject)
            m_sceneManager->destroyManualObject(m_tileDebugObject);
        m_tileDebugObject = nullptr;
    }
}

bool OgreScene::setTileDebugOverlayEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_tileDebugEnabled = enabled;
    rebuildTileDebugOverlay();
    return true;
}


std::vector<PbrMaterialState> OgreScene::modelLayerPbrMaterialStates(
    const QString& layerKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return std::vector<PbrMaterialState> {};
    const ModelResources& resources = it->second;
    // 对外仍按基元返回：块去重了，这里按 primitiveDatablocks 展开回逐基元视图
    // （没有发生共享时它就是恒等映射）。
    if (resources.primitiveDatablocks.empty())
        return resources.pbrMaterialStates;
    std::vector<PbrMaterialState> expanded;
    expanded.reserve(resources.primitiveDatablocks.size());
    for (const std::uint32_t variant : resources.primitiveDatablocks)
        expanded.push_back(variant < resources.pbrMaterialStates.size()
                               ? resources.pbrMaterialStates[variant]
                               : PbrMaterialState {});
    return expanded;
}


LayerRenderState OgreScene::modelLayerRenderState(const QString& layerKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    if (it == m_layers.end())
        return LayerRenderState::Unknown;
    if (it->second.pending)
        return LayerRenderState::Pending;
    if (it->second.failed)
        return LayerRenderState::Failed;
    return LayerRenderState::Ready;
}

void OgreScene::setLayerStateCallback(
    std::function<void(const QString&, LayerRenderState)> callback)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_layerStateCallback = std::move(callback);
}

std::shared_ptr<const ModelMeshDto> OgreScene::modelLayerMesh(
    const QString& layerKey) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    const auto it = m_layers.find(layerKey);
    return it == m_layers.end() ? nullptr : it->second.pickMesh;
}


bool OgreScene::setFeatureHighlightStyle(const FeatureHighlightStyle& style)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!std::isfinite(style.red) || !std::isfinite(style.green) ||
        !std::isfinite(style.blue) || !std::isfinite(style.opacity) ||
        !std::isfinite(style.depthBias) || style.red < 0.0f || style.red > 1.0f ||
        style.green < 0.0f || style.green > 1.0f || style.blue < 0.0f ||
        style.blue > 1.0f || style.opacity < 0.05f || style.opacity > 1.0f ||
        style.depthBias < -100.0f || style.depthBias > 100.0f) return false;
    clearFeatureHighlights();
    m_featureHighlightStyle = style;
    return true;
}

bool OgreScene::setGlobeModeEnabled(bool enabled)
{
    std::vector<std::function<void(bool)>> callbacks;
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        if (m_globeMode == enabled)
            return true;
        if (enabled) {
            if (m_ready && !createGlobeSurface())
                return false;
        } else {
            clearGlobeSurface();
        }
        m_globeMode = enabled;
        // 锚点失效：下一次 setGlobeRenderOrigin 才重新摆瓦片与椭球。
        m_globeAnchorValid = false;
        m_globeRenderOrigin = {};
        applyGlobeAnchorToLayers();
        // 球面下未球面化的平面图层整体隐藏（数据插件重烘焙换网格后再显示）。
        for (auto& [layerKey, resources] : m_layers) {
            static_cast<void>(layerKey);
            applyNodeVisibility(resources);
        }
        if (m_gridObject)
            m_gridObject->setVisible(m_environmentStyle.gridVisible && !m_globeMode);
        if (m_axesObject)
            m_axesObject->setVisible(m_environmentStyle.axesVisible && !m_globeMode);
        callbacks.reserve(m_globeListeners.size());
        for (const auto& entry : m_globeListeners)
            callbacks.push_back(entry.second);
    }
    // 监听者会重启瓦片流会话（stop 里要等异步销毁、重新进渲染锁），
    // 必须在锁外派发，否则死锁。
    for (const auto& callback : callbacks)
        callback(enabled);
    return true;
}

bool OgreScene::globeModeEnabled() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    return m_globeMode;
}

bool OgreScene::globeRenderOrigin(std::array<double, 3>* ecefMeters) const
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_globeMode || !m_globeAnchorValid || !ecefMeters)
        return false;
    *ecefMeters = m_globeRenderOrigin;
    return true;
}

void OgreScene::setGlobeRenderOrigin(const std::array<double, 3>& ecefMeters)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!m_globeMode || !m_ready)
        return;
    if (m_globeAnchorValid && m_globeRenderOrigin == ecefMeters)
        return;
    m_globeRenderOrigin = ecefMeters;
    m_globeAnchorValid = true;
    applyGlobeAnchorToLayers();
    updateGlobeSurface();
}

std::uint64_t OgreScene::addGlobeModeChangeListener(std::function<void(bool)> callback)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (!callback)
        return 0;
    const std::uint64_t token = ++m_globeListenerToken;
    m_globeListeners.emplace(token, std::move(callback));
    return token;
}

void OgreScene::removeGlobeModeChangeListener(std::uint64_t token)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    m_globeListeners.erase(token);
}

void OgreScene::applyGlobeAnchorToLayers()
{
    for (auto& [layerKey, resources] : m_layers) {
        static_cast<void>(layerKey);
        if (!resources.sourceMesh || !resources.sourceMesh->hasContentOrigin)
            continue;
        const std::array<double, 3> base = m_globeMode
            ? GlobeMath::sub(resources.sourceMesh->contentOrigin, m_globeRenderOrigin)
            : std::array<double, 3> {};
        if (resources.globeAnchored == m_globeMode && resources.globeBasePosition == base)
            continue;
        resources.globeAnchored = m_globeMode;
        resources.globeBasePosition = base;
        // pending 图层没有 rootNode，这里只更新记录；uploadLayer 会原样带走。
        applyLayerPosition(resources);
    }
}

namespace {
// 球面底图网格：96 段 × 48 环。等距圆柱 UV 直接按经纬度取样，与默认底图对齐。
constexpr std::uint32_t kGlobeSegments = 96;
constexpr std::uint32_t kGlobeRings = 48;

// 球面底图采样：经度方向回绕，纬度到极点夹取，避免两极采到对侧。
Ogre::HlmsSamplerblock globeBasemapSampler()
{
    Ogre::HlmsSamplerblock sampler;
    sampler.setFiltering(Ogre::TFO_ANISOTROPIC);
    sampler.mU = Ogre::TAM_WRAP;
    sampler.mV = Ogre::TAM_CLAMP;
    sampler.mW = Ogre::TAM_CLAMP;
    sampler.mMaxAnisotropy = 8.0f;
    return sampler;
}
} // namespace

Ogre::TextureGpu* OgreScene::createGlobeBasemapTexture()
{
    if (!m_root)
        return nullptr;
    QImage source;
    if (!m_globeCustomBasemapRgba.empty() && m_globeCustomBasemapWidth > 0 &&
        m_globeCustomBasemapHeight > 0) {
        // 在线影像：数据已是 RGBA8 等距圆柱，拷贝一份交给 QImage 统一走 mip 铺设。
        source = QImage(m_globeCustomBasemapRgba.data(), m_globeCustomBasemapWidth,
                        m_globeCustomBasemapHeight, QImage::Format_RGBA8888)
                     .copy();
    } else {
        // 默认底图随应用部署；环境变量可覆盖，便于替换素材或排查贴图问题。
        QString path = qEnvironmentVariable("VIEW3D_GLOBE_BASEMAP");
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            path = QCoreApplication::applicationDirPath() +
                QStringLiteral("/resources/globe_basemap.jpg");
        }
        QImage loaded(path);
        if (loaded.isNull()) {
            std::cerr << "VIEW3D_GLOBE_BASEMAP_MISSING path=" << path.toStdString()
                      << std::endl;
            return nullptr;
        }
        source = loaded.convertToFormat(QImage::Format_RGBA8888);
    }
    // 只接受等距圆柱（2:1）：其他比例按经纬度取样会把大陆拉歪。
    if (source.height() <= 0 || source.width() != source.height() * 2) {
        std::cerr << "VIEW3D_GLOBE_BASEMAP_INVALID size=" << source.width() << "x"
                  << source.height() << std::endl;
        return nullptr;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(source.width());
    const std::uint32_t height = static_cast<std::uint32_t>(source.height());
    Ogre::TextureGpuManager* textureManager =
        m_root->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu* texture = textureManager->createTexture(
        "View3DGlobeBasemap" + Ogre::StringConverter::toString(nextOgreResourceSerial()),
        Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture,
        Ogre::TextureTypes::Type2D);
    texture->setResolution(width, height);
    // 与球面纯色一样按原始值输出：这条渲染链路的颜色不做 sRGB 编码，
    // 贴图若声明成 sRGB 会在采样时多解一次 gamma，整球发黑。
    texture->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
    std::uint32_t mipCount = 1;
    for (std::uint32_t w = width, h = height; w > 1 || h > 1;
         w = std::max(1u, w / 2u), h = std::max(1u, h / 2u))
        ++mipCount;
    texture->setNumMipmaps(static_cast<Ogre::uint8>(mipCount));
    texture->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    // 手工铺整条 mip 链：远景整球时线性过滤只剩几个像素，没有 mip 会闪。
    Ogre::StagingTexture* staging = nullptr;
    try {
        staging = textureManager->getStagingTexture(width, height, 1u, 1u,
                                                    Ogre::PFG_RGBA8_UNORM);
        for (std::uint32_t mip = 0; mip < mipCount; ++mip) {
            const std::uint32_t mipWidth = std::max(1u, width >> mip);
            const std::uint32_t mipHeight = std::max(1u, height >> mip);
            const QImage level = mip == 0
                ? source
                : source.scaled(static_cast<int>(mipWidth), static_cast<int>(mipHeight),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGBA8888);
            staging->startMapRegion();
            Ogre::TextureBox box = staging->mapRegion(mipWidth, mipHeight, 1u, 1u,
                                                      Ogre::PFG_RGBA8_UNORM);
            box.copyFrom(level.constBits(), mipWidth, mipHeight, mipWidth * 4u);
            staging->stopMapRegion();
            staging->upload(box, texture, static_cast<Ogre::uint8>(mip));
        }
        textureManager->removeStagingTexture(staging);
        staging = nullptr;
    } catch (...) {
        if (staging) {
            try {
                textureManager->removeStagingTexture(staging);
            } catch (...) {
            }
        }
        textureManager->destroyTexture(texture);
        throw;
    }
    return texture;
}

bool OgreScene::createGlobeSurface()
{
    if (!m_sceneManager || !m_root)
        return false;
    if (m_globeObject)
        return true;
    try {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (!unlit)
            return false;
        Ogre::HlmsMacroblock macroblock;
        // 底面只用来挡住地球背面：不剔三角形（绕向不保证），靠深度写遮挡远端瓦片。
        macroblock.mCullMode = Ogre::CULL_NONE;
        m_globeDatablockName = "View3DGlobe" +
            Ogre::StringConverter::toString(nextOgreResourceSerial());
        m_globeDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(
            unlit->createDatablock(m_globeDatablockName, m_globeDatablockName, macroblock,
                                   Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        // 默认底图优先：贴图缺失时退回纯色球面，球面模式本身不受影响。
        m_globeBasemapTexture = createGlobeBasemapTexture();
        if (m_globeBasemapTexture) {
            const Ogre::HlmsSamplerblock sampler = globeBasemapSampler();
            m_globeDatablock->setUseColour(false);
            m_globeDatablock->setTexture(0, m_globeBasemapTexture, &sampler);
        } else {
            m_globeDatablock->setUseColour(true);
            m_globeDatablock->setColour(Ogre::ColourValue(0.10f, 0.14f, 0.20f, 1.0f));
        }

        m_globeVerticesEcef.reserve(
            static_cast<std::size_t>(kGlobeSegments + 1) * (kGlobeRings + 1) * 3);
        for (std::uint32_t row = 0; row <= kGlobeRings; ++row) {
            const double lat = -90.0 + 180.0 * static_cast<double>(row) / kGlobeRings;
            for (std::uint32_t col = 0; col <= kGlobeSegments; ++col) {
                const double lon = -180.0 + 360.0 * static_cast<double>(col) / kGlobeSegments;
                const GlobeMath::Vec3 ecef = GlobeMath::geodeticToEcef(lon, lat, 0.0);
                m_globeVerticesEcef.insert(m_globeVerticesEcef.end(),
                                           ecef.begin(), ecef.end());
            }
        }
        m_globeIndices.reserve(static_cast<std::size_t>(kGlobeSegments) * kGlobeRings * 6);
        for (std::uint32_t row = 0; row < kGlobeRings; ++row) {
            for (std::uint32_t col = 0; col < kGlobeSegments; ++col) {
                const std::uint32_t a = row * (kGlobeSegments + 1) + col;
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + (kGlobeSegments + 1);
                const std::uint32_t d = c + 1;
                m_globeIndices.insert(m_globeIndices.end(), {a, c, b, b, c, d});
            }
        }
        m_globeObject = m_sceneManager->createManualObject();
        m_globeObject->setCastShadows(false);
        m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)->attachObject(m_globeObject);
        m_globeObject->begin(m_globeDatablockName, Ogre::OT_TRIANGLE_LIST);
        emitGlobeGeometry();
        m_globeObject->end()->setDatablock(m_globeDatablock);
        return true;
    } catch (const Ogre::Exception&) {
        clearGlobeSurface();
        return false;
    }
}

void OgreScene::emitGlobeGeometry()
{
    if (!m_globeObject)
        return;
    const std::size_t vertexCount = m_globeVerticesEcef.size() / 3;
    const std::uint32_t stride = kGlobeSegments + 1;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        // ECEF 双精度先减锚点再落 float：绝对值 ~6.4e6 直接转 float 会抖米级。
        m_globeObject->position(
            static_cast<Ogre::Real>(m_globeVerticesEcef[i * 3] - m_globeRenderOrigin[0]),
            static_cast<Ogre::Real>(m_globeVerticesEcef[i * 3 + 1] -
                                    m_globeRenderOrigin[1]),
            static_cast<Ogre::Real>(m_globeVerticesEcef[i * 3 + 2] -
                                    m_globeRenderOrigin[2]));
        // 等距圆柱 UV：u 随经度 0→1，纹理顶部是北极，v 由南往北 1→0。
        const std::uint32_t col = static_cast<std::uint32_t>(i) % stride;
        const std::uint32_t row = static_cast<std::uint32_t>(i) / stride;
        m_globeObject->textureCoord(
            static_cast<Ogre::Real>(col) / static_cast<Ogre::Real>(kGlobeSegments),
            static_cast<Ogre::Real>(1.0f - static_cast<Ogre::Real>(row) /
                                              static_cast<Ogre::Real>(kGlobeRings)));
    }
    for (std::uint32_t index : m_globeIndices)
        m_globeObject->index(index);
}

void OgreScene::updateGlobeSurface()
{
    if (!m_globeObject || m_globeVerticesEcef.empty())
        return;
    m_globeObject->beginUpdate(0);
    emitGlobeGeometry();
    m_globeObject->end();
}

bool OgreScene::setGlobeBasemapImage(int width, int height,
                                     const std::vector<unsigned char>& rgba8)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
    if (width <= 0 || height <= 0 || width != height * 2 ||
        rgba8.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                4u)
        return false;
    m_globeCustomBasemapWidth = width;
    m_globeCustomBasemapHeight = height;
    m_globeCustomBasemapRgba = rgba8;
    if (!m_globeDatablock || !m_globeObject) {
        // 球面表面还没建：数据已保存，createGlobeSurface 会直接用它。
        return true;
    }
    Ogre::TextureGpu* texture = createGlobeBasemapTexture();
    if (!texture)
        return false;
    if (m_globeBasemapTexture) {
        if (auto* textureManager = m_root->getRenderSystem()->getTextureGpuManager())
            textureManager->destroyTexture(m_globeBasemapTexture);
    }
    m_globeBasemapTexture = texture;
    const Ogre::HlmsSamplerblock sampler = globeBasemapSampler();
    m_globeDatablock->setUseColour(false);
    m_globeDatablock->setSamplerblock(0, sampler);
    m_globeDatablock->setTexture(0, texture);
    return true;
}

void OgreScene::clearGlobeSurface()
{    if (m_sceneManager && m_globeObject)
        m_sceneManager->destroyManualObject(m_globeObject);
    m_globeObject = nullptr;
    if (m_root) {
        Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (unlit && m_globeDatablock && !m_globeDatablockName.empty())
            unlit->destroyDatablock(Ogre::IdString(m_globeDatablockName));
    }
    m_globeDatablock = nullptr;
    if (m_globeBasemapTexture) {
        if (m_root) {
            if (auto* textureManager = m_root->getRenderSystem()->getTextureGpuManager())
                textureManager->destroyTexture(m_globeBasemapTexture);
        }
        m_globeBasemapTexture = nullptr;
    }
    m_globeDatablockName.clear();
    m_globeVerticesEcef.clear();
    m_globeIndices.clear();
}


} // namespace View3D
