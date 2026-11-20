#include "SceneLayerService.h"

#include <IRenderScene.h>
#include <LayerGroup.h>
#include <ModelLayer.h>
#include <ModelMesh.h>
#include <GeoLayer.h>

#include <QList>
#include <QVariantMap>

namespace View3DPlugins {

SceneLayerService::SceneLayerService(View3D::IRenderScene& scene) : m_scene(scene)
{
    m_scene.setLayerStateCallback([this](const QString& renderKey, View3D::LayerRenderState state) {
        applySceneRenderState(renderKey, state);
    });
}

SceneLayerService::~SceneLayerService()
{
    m_scene.setLayerStateCallback({});
}

View3D::LayerId SceneLayerService::addGroup(const QString& name,
                                            const View3D::LayerId& parentGroup)
{
    const QString normalized = name.trimmed().isEmpty() ? QStringLiteral("组图层")
                                                        : name.trimmed();
    View3D::LayerId parent = parentGroup;
    if (!parent.isNull()) {
        const auto parentLayer = m_layers.layer(parent);
        if (!parentLayer || parentLayer->type() != View3D::LayerType::Group)
            parent = {};
    }
    for (const View3D::LayerId& childId : m_layers.childLayers(parent)) {
        const auto child = m_layers.layer(childId);
        if (child && child->type() == View3D::LayerType::Group &&
            child->name() == normalized)
            return childId;
    }
    auto group = std::make_shared<View3D::LayerGroup>(normalized);
    const View3D::LayerId id = group->id();
    if (!m_layers.addLayer(group, parent))
        return {};
    notify(LayerChangeKind::Added, id);
    return id;
}

View3D::LayerId SceneLayerService::addModelLayer(
    const QString& name, const QString& renderKey,
    const std::shared_ptr<const View3D::ModelMeshDto>& mesh,
    const View3D::LayerId& parentGroup, const QString& sourceCrs)
{
    if (!mesh || renderKey.isEmpty())
        return {};

    const auto existing = m_layersByRenderKey.find(renderKey);
    if (existing != m_layersByRenderKey.end())
        removeLayer(existing.value());

    View3D::LayerId parent = parentGroup;
    if (!parent.isNull()) {
        const auto parentLayer = m_layers.layer(parent);
        if (!parentLayer || parentLayer->type() != View3D::LayerType::Group)
            parent = {};
    }

    const std::shared_ptr<const View3D::ModelMeshDto> sharedMesh = mesh;
    if (!m_scene.addModelLayer(renderKey, sharedMesh))
        return {};

    QVariantMap statistics;
    statistics.insert(QStringLiteral("vertices"),
                      static_cast<qulonglong>(mesh->vertices.size()));
    statistics.insert(QStringLiteral("indices"),
                      static_cast<qulonglong>(mesh->indices.size()));
    statistics.insert(QStringLiteral("primitives"),
                      static_cast<int>(mesh->primitives.size()));
    auto layer = std::make_shared<View3D::ModelLayer>(
        name.trimmed().isEmpty() ? QStringLiteral("图层") : name.trimmed(),
        renderKey, statistics);
    if (!sourceCrs.trimmed().isEmpty())
        layer->setSourceCrs(sourceCrs);
    const View3D::LayerId id = layer->id();
    if (!parentChainVisible(parent)) {
        layer->setVisible(false);
        m_scene.setModelLayerVisible(renderKey, false);
    }
    if (!m_layers.addLayer(layer, parent)) {
        m_scene.removeModelLayer(renderKey);
        return {};
    }
    m_renderKeys.insert(id, renderKey);
    m_layersByRenderKey.insert(renderKey, id);
    layer->setLoadState(View3D::LoadState::Queued);
    notify(LayerChangeKind::Added, id);
    return id;
}

View3D::LayerId SceneLayerService::addTileLayer(const QString& name,
                                                const QString& tileGroupKey,
                                                const View3D::LayerId& parentGroup,
                                                const QString& sourceCrs)
{
    if (tileGroupKey.isEmpty())
        return {};

    const auto existing = m_layersByTileGroup.find(tileGroupKey);
    if (existing != m_layersByTileGroup.end())
        removeLayer(existing.value());

    View3D::LayerId parent = parentGroup;
    if (!parent.isNull()) {
        const auto parentLayer = m_layers.layer(parent);
        if (!parentLayer || parentLayer->type() != View3D::LayerType::Group)
            parent = {};
    }

    QVariantMap statistics;
    statistics.insert(QStringLiteral("tileGroup"), tileGroupKey);
    auto layer = std::make_shared<View3D::ModelLayer>(
        name.trimmed().isEmpty() ? QStringLiteral("倾斜摄影图层") : name.trimmed(),
        tileGroupKey, statistics, View3D::LayerType::Tiled);
    if (!sourceCrs.trimmed().isEmpty())
        layer->setSourceCrs(sourceCrs);
    const View3D::LayerId id = layer->id();
    layer->setLoadState(View3D::LoadState::Loading);
    if (!parentChainVisible(parent)) {
        layer->setVisible(false);
        m_scene.setTileGroupVisible(tileGroupKey, false);
    }
    if (!m_layers.addLayer(layer, parent))
        return {};
    m_tileGroups.insert(id, tileGroupKey);
    m_layersByTileGroup.insert(tileGroupKey, id);
    notify(LayerChangeKind::Added, id);
    return id;
}

View3D::LayerId SceneLayerService::addImageryLayer(const QString& name,
                                                  const QString& renderKey,
                                                  const View3D::LayerId& parentGroup)
{
    return addGeoLayer(name, renderKey, parentGroup, View3D::LayerType::Imagery);
}

View3D::LayerId SceneLayerService::addFeatureLayer(const QString& name,
                                                  const QString& renderKey,
                                                  const View3D::LayerId& parentGroup)
{
    return addGeoLayer(name, renderKey, parentGroup, View3D::LayerType::Feature);
}

// 地理图层的登记：几何由 GeoPlugin 的服务提交到渲染场景，这里只登记图层元数据。
// 渲染 key 仍走 m_renderKeys 同一条路由，勾选/透明度/偏移/样式和
// 组级可见性才能和普通模型图层一致地生效。
View3D::LayerId SceneLayerService::addGeoLayer(const QString& name,
                                              const QString& renderKey,
                                              const View3D::LayerId& parentGroup,
                                              View3D::LayerType type)
{
    if (renderKey.isEmpty())
        return {};
    const auto existing = m_layersByRenderKey.find(renderKey);
    if (existing != m_layersByRenderKey.end())
        removeLayer(existing.value());

    View3D::LayerId parent = parentGroup;
    if (!parent.isNull()) {
        const auto parentLayer = m_layers.layer(parent);
        if (!parentLayer || parentLayer->type() != View3D::LayerType::Group)
            parent = {};
    }

    QVariantMap statistics;
    statistics.insert(QStringLiteral("geoKey"), renderKey);
    auto layer = std::make_shared<View3D::GeoLayer>(name.trimmed().isEmpty()
                                                        ? QStringLiteral("地理图层")
                                                        : name.trimmed(),
                                                    renderKey, statistics, type);
    const View3D::LayerId id = layer->id();
    layer->setLoadState(View3D::LoadState::Loading);
    if (!parentChainVisible(parent)) {
        layer->setVisible(false);
        m_scene.setModelLayerVisible(renderKey, false);
    }
    if (!m_layers.addLayer(layer, parent))
        return {};
    m_renderKeys.insert(id, renderKey);
    m_layersByRenderKey.insert(renderKey, id);
    notify(LayerChangeKind::Added, id);
    return id;
}

void SceneLayerService::collectSubtree(const View3D::LayerId& layerId,
                                       QList<View3D::LayerId>* ids) const
{
    if (!ids)
        return;
    ids->append(layerId);
    for (const View3D::LayerId& childId : m_layers.childLayers(layerId))
        collectSubtree(childId, ids);
}

bool SceneLayerService::removeLayer(const View3D::LayerId& layerId)
{
    if (!m_layers.layer(layerId))
        return false;
    QList<View3D::LayerId> removed;
    collectSubtree(layerId, &removed);
    for (const View3D::LayerId& id : removed) {
        const QString group = m_tileGroups.take(id);
        if (!group.isEmpty()) {
            m_layersByTileGroup.remove(group);
            m_scene.clearTileGroup(group);
            continue;
        }
        const QString key = m_renderKeys.take(id);
        if (!key.isEmpty()) {
            m_scene.removeModelLayer(key);
            m_layersByRenderKey.remove(key);
        }
    }
    m_layers.removeLayer(layerId);
    notify(LayerChangeKind::Removed, layerId);
    return true;
}

void SceneLayerService::clear()
{
    for (auto it = m_renderKeys.cbegin(); it != m_renderKeys.cend(); ++it)
        m_scene.removeModelLayer(it.value());
    for (auto it = m_tileGroups.cbegin(); it != m_tileGroups.cend(); ++it)
        m_scene.clearTileGroup(it.value());
    m_renderKeys.clear();
    m_layersByRenderKey.clear();
    m_tileGroups.clear();
    m_layersByTileGroup.clear();
    m_layers.clear();
    notify(LayerChangeKind::Reset);
}

void SceneLayerService::applyGroupVisibility(const View3D::LayerId& groupId, bool visible)
{
    const QString group = m_tileGroups.value(groupId);
    if (!group.isEmpty()) {
        m_scene.setTileGroupVisible(group, visible);
    } else {
        const QString key = m_renderKeys.value(groupId);
        if (!key.isEmpty())
            m_scene.setModelLayerVisible(key, visible);
    }
    for (const View3D::LayerId& childId : m_layers.childLayers(groupId))
        applyGroupVisibility(childId, visible);
}

bool SceneLayerService::replaceModelLayerGeometry(
    const View3D::LayerId& layerId,
    const std::shared_ptr<const View3D::ModelMeshDto>& mesh)
{
    if (layerId.isNull() || !mesh || mesh->vertices.empty() || mesh->primitives.empty())
        return false;
    const auto keyIt = m_renderKeys.find(layerId);
    if (keyIt == m_renderKeys.end())
        return false;
    const auto layer = m_layers.layer(layerId);
    auto modelLayer = std::dynamic_pointer_cast<View3D::ModelLayer>(layer);
    if (!modelLayer)
        return false;
    if (!m_scene.updateModelLayerMesh(*keyIt, mesh))
        return false;
    // 统计行跟着新网格走；Updated 通知让属性面板与图层树刷新（渲染就绪还会再来一次，
    // 两次都是同一图层，面板按当前层重读，重复无害）。
    QVariantMap statistics = modelLayer->statistics();
    statistics.insert(QStringLiteral("vertices"),
                      static_cast<qulonglong>(mesh->vertices.size()));
    statistics.insert(QStringLiteral("indices"),
                      static_cast<qulonglong>(mesh->indices.size()));
    statistics.insert(QStringLiteral("primitives"), static_cast<int>(mesh->primitives.size()));
    modelLayer->setStatistics(statistics);
    notify(LayerChangeKind::Updated, layerId);
    return true;
}

bool SceneLayerService::setLayerVisible(const View3D::LayerId& layerId, bool visible)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    if (layer->type() == View3D::LayerType::Group) {
        if (!m_layers.setGroupVisible(layerId, visible))
            return false;
        applyGroupVisibility(layerId, visible);
    } else {
        layer->setVisible(visible);
        const QString group = m_tileGroups.value(layerId);
        if (!group.isEmpty()) {
            m_scene.setTileGroupVisible(group, visible);
        } else {
            const QString key = m_renderKeys.value(layerId);
            if (!key.isEmpty())
                m_scene.setModelLayerVisible(key, visible);
        }
    }
    notify(LayerChangeKind::Updated, layerId);
    return true;
}

bool SceneLayerService::setLayerOpacity(const View3D::LayerId& layerId, float opacity)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    layer->setOpacity(opacity);
    const QString group = m_tileGroups.value(layerId);
    if (!group.isEmpty())
        return m_scene.setTileGroupOpacity(group, opacity);
    const QString key = m_renderKeys.value(layerId);
    return !key.isEmpty() && m_scene.setModelLayerOpacity(key, opacity);
}

bool SceneLayerService::setLayerOffset(const View3D::LayerId& layerId,
                                       const std::array<double, 3>& offset)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    const QString group = m_tileGroups.value(layerId);
    if (!group.isEmpty())
        return m_scene.setTileGroupOffset(group, offset);
    const QString key = m_renderKeys.value(layerId);
    return !key.isEmpty() && m_scene.setModelLayerOffset(key, offset);
}

bool SceneLayerService::setLayerVisibleDistance(const View3D::LayerId& layerId,
                                                double minimumDistance,
                                                double maximumDistance)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    // 瓦片图层的层级由数据内嵌阈值决定，不接受额外的距离窗口。
    if (m_tileGroups.contains(layerId))
        return false;
    layer->setVisibleDistance(minimumDistance, maximumDistance);
    const QString key = m_renderKeys.value(layerId);
    return !key.isEmpty() &&
        m_scene.setModelLayerDistanceLimits(key, minimumDistance, maximumDistance);
}

bool SceneLayerService::setLayerStyle(const View3D::LayerId& layerId,
                                      const std::array<float, 3>& tint,
                                      bool edgeLinesVisible, float brightness,
                                      float contrast)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    if (m_tileGroups.contains(layerId))
        return false;
    layer->setStyle(tint, edgeLinesVisible, brightness, contrast);
    const QString key = m_renderKeys.value(layerId);
    return !key.isEmpty() && m_scene.setModelLayerStyle(key, tint, edgeLinesVisible,
                                                       brightness, contrast);
}

void SceneLayerService::applySceneRenderState(const QString& renderKey,
                                               View3D::LayerRenderState state)
{
    if (renderKey.startsWith(QLatin1String("tile:"))) {
        // 瓦片键形如 tile:<组>/<瓦片>，单个瓦片的就绪只反映为图层“加载中”。
        const QString rest = renderKey.mid(5);
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash <= 0)
            return;
        const View3D::LayerId tileLayerId =
            m_layersByTileGroup.value(rest.left(slash));
        const auto tileLayer =
            std::dynamic_pointer_cast<View3D::ModelLayer>(m_layers.layer(tileLayerId));
        if (!tileLayer)
            return;
        tileLayer->setLoadState(state == View3D::LayerRenderState::Ready
                                    ? View3D::LoadState::PartiallyReady
                                    : View3D::LoadState::Loading);
        notify(LayerChangeKind::Updated, tileLayerId);
        return;
    }

    const View3D::LayerId layerId = m_layersByRenderKey.value(renderKey);
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return;
    View3D::LoadState loadState = View3D::LoadState::Loading;
    switch (state) {
    case View3D::LayerRenderState::Pending:
        loadState = View3D::LoadState::Queued;
        break;
    case View3D::LayerRenderState::Ready:
        loadState = View3D::LoadState::Ready;
        break;
    case View3D::LayerRenderState::Failed:
        loadState = View3D::LoadState::Failed;
        break;
    case View3D::LayerRenderState::Unknown:
        return;
    }
    // 模型图层与地理图层各自存就绪状态，但都来自同一条渲染状态回调。
    auto model = std::dynamic_pointer_cast<View3D::ModelLayer>(layer);
    auto geo = std::dynamic_pointer_cast<View3D::GeoLayer>(layer);
    if (model)
        model->setLoadState(loadState);
    else if (geo)
        geo->setLoadState(loadState);
    else
        return;
    notify(LayerChangeKind::Updated, layerId);
}

bool SceneLayerService::parentChainVisible(const View3D::LayerId& layerId) const
{
    View3D::LayerId current = layerId;
    while (!current.isNull()) {
        const auto layer = m_layers.layer(current);
        if (layer && !layer->isVisible())
            return false;
        current = m_layers.parentLayer(current);
    }
    return true;
}

void SceneLayerService::addChangeCallback(ChangeCallback callback)
{
    if (callback)
        m_changed.push_back(std::move(callback));
}

void SceneLayerService::notify(LayerChangeKind kind, const View3D::LayerId& id)
{
    const LayerChange change { kind, id };
    for (const ChangeCallback& callback : m_changed)
        callback(change);
}

QString SceneLayerService::renderKey(const View3D::LayerId& layerId) const
{
    const QString group = m_tileGroups.value(layerId);
    return group.isEmpty() ? m_renderKeys.value(layerId) : group;
}

View3D::LayerId SceneLayerService::layerByRenderKey(const QString& renderKey) const
{
    const auto direct = m_layersByRenderKey.constFind(renderKey);
    if (direct != m_layersByRenderKey.constEnd())
        return direct.value();
    // 瓦片渲染键形如 tile:<组>/<瓦片>：一个数据集只有一个图层，归到组键上。
    // 组键自己带路径分隔符（tileset:C:/data/…/tileset.json），按第一个 '/' 切会把
    // 组键截断，因此这里按"最长组键前缀"匹配。
    static const QLatin1String kTilePrefix("tile:");
    if (!renderKey.startsWith(kTilePrefix))
        return {};
    const QString rest = renderKey.mid(kTilePrefix.size());
    QString matchedGroup;
    for (auto it = m_layersByTileGroup.constBegin(); it != m_layersByTileGroup.constEnd();
         ++it) {
        const QString& group = it.key();
        const bool matches = rest == group ||
            (rest.startsWith(group) && rest.size() > group.size() &&
             rest.at(group.size()) == QLatin1Char('/'));
        if (matches && group.size() > matchedGroup.size())
            matchedGroup = group;
    }
    return matchedGroup.isEmpty() ? View3D::LayerId {}
                                  : m_layersByTileGroup.value(matchedGroup);
}

QString SceneLayerService::layerName(const View3D::LayerId& layerId) const
{
    const auto layer = m_layers.layer(layerId);
    return layer ? layer->name() : QString();
}

QString SceneLayerService::layerSourceCrs(const View3D::LayerId& layerId) const
{
    const auto layer = m_layers.layer(layerId);
    return layer ? layer->sourceCrs() : QString();
}

bool SceneLayerService::setLayerReferenceFrame(const View3D::LayerId& layerId,
                                               const View3D::ILayer::ReferenceFrame& frame)
{
    const auto layer = m_layers.layer(layerId);
    if (!layer)
        return false;
    layer->setReferenceFrame(frame);
    // 面板按图层变化刷新：3D Tiles 的框架要等首个瓦片定出原点后才回填。
    notify(LayerChangeKind::Updated, layerId);
    return true;
}

View3D::ILayer::ReferenceFrame SceneLayerService::layerReferenceFrame(
    const View3D::LayerId& layerId) const
{
    const auto layer = m_layers.layer(layerId);
    return layer ? layer->referenceFrame() : View3D::ILayer::ReferenceFrame {};
}

} // namespace View3DPlugins
