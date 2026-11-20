#pragma once

#include "OgrePbsReflection.h"
#include "OgreRuntime.h"

#include <IRenderScene.h>
#include <RenderTypes.h>

#include <QList>
#include <QQueue>
#include <QString>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace Ogre {
class Camera;
class HlmsPbsDatablock;
class HlmsUnlitDatablock;
class Light;
class ManualObject;
class Root;
class SceneManager;
class SceneNode;
class TextureGpu;
class Vector3;
}

namespace View3D {

class OgreRuntime;
struct ModelMeshDto;
struct ModelTextureDto;

// 共享场景：拥有 SceneManager、模型/瓦片资源、显示与环境状态。
// 视口只通过 IRenderScene 访问，多个视口共享同一份 GPU 资源。
class OgreScene final : public IRenderScene {
  public:
    explicit OgreScene(OgreRuntime& runtime);
    ~OgreScene() override;

    bool ensureReady(const QString& runtimeDirectory);
    void shutdown();

    bool ready() const noexcept override;
    Ogre::SceneManager* sceneManager() const noexcept
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_sceneManager;
    }
    Ogre::Root* root() const noexcept
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_root;
    }
    void processPendingUploads();

    // 视口可见性位：共享场景用位标记“哪些视口应该看到这个对象”。
    // 返回 0 表示位已用尽，此时该视口退化为看到全部内容。
    std::uint32_t acquireViewSlot(Ogre::Camera* camera);
    void releaseViewSlot(std::uint32_t viewBit);
    void updateDistanceVisibility();
    static std::uint32_t allViewsMask() noexcept;

    bool addModelLayer(const QString& layerKey,
                       std::shared_ptr<const ModelMeshDto> mesh) override;
    bool updateModelLayerMesh(const QString& layerKey,
                              std::shared_ptr<const ModelMeshDto> mesh) override;
    bool removeModelLayer(const QString& layerKey) override;
    void clearModelLayers() override;
    bool setModelLayerVisible(const QString& layerKey, bool visible) override;
    bool isModelLayerVisible(const QString& layerKey) const override;
    bool setModelLayerOpacity(const QString& layerKey, float opacity) override;
    float modelLayerOpacity(const QString& layerKey) const override;
    bool setModelLayerOffset(const QString& layerKey,
                             const std::array<double, 3>& offset) override;
    bool modelLayerOffset(const QString& layerKey,
                          std::array<double, 3>* offset) const override;
    bool setModelLayerDistanceLimits(const QString& layerKey, double minimumDistance,
                                     double maximumDistance) override;
    bool setModelLayerStyle(const QString& layerKey,
                            const std::array<float, 3>& tint,
                            bool edgeLinesVisible,
                            float brightness,
                            float contrast) override;
    bool selectModelLayerRange(const QString& layerKey, std::uint32_t firstPrimitive,
                               std::uint32_t primitiveCount) override;
    QList<QString> modelLayerKeys() const override;
    std::shared_ptr<const ModelMeshDto> modelLayerMesh(
        const QString& layerKey) const override;
    LayerRenderState modelLayerRenderState(const QString& layerKey) const override;
    void setLayerStateCallback(
        std::function<void(const QString&, LayerRenderState)> callback) override;
    bool modelLayerBounds(const QString& layerKey, RenderBounds* bounds) const override;
    bool modelLayerNodeBounds(const QString& layerKey, std::uint32_t nodeIndex,
                              RenderBounds* bounds) const override;
    std::vector<PbrMaterialState> modelLayerPbrMaterialStates(
        const QString& layerKey) const override;

    bool addTile(const QString& tileGroupKey, const QString& tileKey,
                 std::shared_ptr<const ModelMeshDto> mesh) override;
    bool removeTile(const QString& tileGroupKey, const QString& tileKey) override;
    bool setTileVisible(const QString& tileGroupKey, const QString& tileKey,
                        bool visible) override;
    bool setTileNodeMask(const QString& tileGroupKey, const QString& tileKey,
                         std::uint64_t nodeMask) override;
    bool isTileVisible(const QString& tileGroupKey,
                       const QString& tileKey) const override;
    LayerRenderState tileRenderState(const QString& tileGroupKey,
                                     const QString& tileKey) const override;
    bool setTileOpacity(const QString& tileGroupKey, const QString& tileKey,
                        float opacity) override;
    bool setTileOffset(const QString& tileGroupKey, const QString& tileKey,
                       const std::array<double, 3>& offset) override;
    float tileOpacity(const QString& tileGroupKey, const QString& tileKey) const override;
    bool tilesHaveOpacity(const QString& tileGroupKey, float opacity) const override;
    bool tilesUseTransparency(const QString& tileGroupKey, bool transparent) const override;
    std::uint32_t tileCount(const QString& tileGroupKey) const noexcept override;
    bool tileGroupBounds(const QString& tileGroupKey, RenderBounds* bounds) const override;
    bool setTileGroupVisible(const QString& tileGroupKey, bool visible) override;
    bool setTileGroupOpacity(const QString& tileGroupKey, float opacity) override;
    bool setTileGroupOffset(const QString& tileGroupKey,
                            const std::array<double, 3>& offset) override;
    bool setTileGroupPlaneTransform(const QString& tileGroupKey, double yawDegrees,
                                    double horizontalScale,
                                    const std::array<double, 3>& placementOffset) override;
    bool clearTileGroup(const QString& tileGroupKey) override;
    bool setTileDebugOverlayEnabled(bool enabled) override;
    bool tileDebugOverlayEnabled() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_tileDebugEnabled;
    }

    bool pickRay(const std::array<double, 3>& origin,
                 const std::array<double, 3>& direction,
                 ViewportPickResult* result) const override;

    bool highlightModelFeature(const QString& layerKey, std::int32_t featureTableIndex,
                               std::int64_t featureId) override;
    void clearFeatureHighlights() override;
    bool setFeatureHighlightStyle(const FeatureHighlightStyle& style) override;
    std::uint32_t highlightedFeatureTriangleCount() const noexcept override;

    bool visibleBounds(RenderBounds* bounds) const override;

    void setRenderBudgetMaxVertices(std::uint64_t maximum) override;
    std::uint64_t renderBudgetMaxVertices() const noexcept override;
    std::uint64_t renderBudgetCurrentVertices() const noexcept override;
    std::uint64_t registeredVertexCount() const noexcept override;
    std::uint64_t registeredIndexCount() const noexcept override;
    std::uint64_t registeredTextureCount() const noexcept override;
    std::uint32_t registeredSceneNodeCount() const noexcept override;
    std::uint32_t registeredRenderObjectCount() const noexcept override;

    bool setDisplayMode(ViewportDisplayMode mode) override;
    ViewportDisplayMode displayMode() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_displayMode;
    }
    bool setGridVisible(bool visible) override;
    bool gridVisible() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_environmentStyle.gridVisible;
    }
    bool setAxesVisible(bool visible) override;
    bool axesPlanar() const noexcept override { return m_axesPlanar; }
    bool setAxesPlanar(bool planar) override;
    bool axesVisible() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_environmentStyle.axesVisible;
    }
    bool setEnvironmentStyle(const ViewportEnvironmentStyle& style) override;
    ViewportEnvironmentStyle environmentStyle() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_environmentStyle;
    }
    bool setLightingStyle(const ViewportLightingStyle& style) override;
    ViewportLightingStyle lightingStyle() const noexcept override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        return m_lightingStyle;
    }
    bool setAmbientLightVisible(bool visible) override;
    bool setMainLightVisible(bool visible) override;

    bool setGlobeModeEnabled(bool enabled) override;
    bool globeModeEnabled() const noexcept override;
    bool globeRenderOrigin(std::array<double, 3>* ecefMeters) const override;
    std::uint64_t addGlobeModeChangeListener(
        std::function<void(bool enabled)> callback) override;
    void removeGlobeModeChangeListener(std::uint64_t token) override;
    // 渲染线程每帧调用：把场景锚到给定 ECEF 原点（仅球面模式生效）。
    // 原点未变时脏标记跳过，静止零开销。
    void setGlobeRenderOrigin(const std::array<double, 3>& ecefMeters);
    // 在线影像球面底图：RGBA8 等距圆柱（2:1，首行北极）。表面未创建时只保存。
    bool setGlobeBasemapImage(int width, int height,
                              const std::vector<unsigned char>& rgba8) override;

  private:
    struct ModelResources {
        std::vector<Ogre::ManualObject*> objects;
        std::vector<std::uint32_t> objectNodeIndices;
        Ogre::SceneNode* rootNode = nullptr;
        std::vector<Ogre::SceneNode*> sceneNodes;
        // 同一瓦片内按 PagedLOD 节点控制可见性：bit i ↔ sourceNodeIndex==i。
        std::uint64_t nodeMask = ~0ull;
        std::vector<Ogre::TextureGpu*> textures;
        // 按外观签名去重的 datablock 表（本图层作用域）。以前每个基元建一块，
        // 98,845 基元的 3DS 厂区模型在销毁第 ~549 块时直接 fastfail；同一图层里
        // 不同材质通常只有百来个，去重后销毁/遍历/绑定都回到正常量级。
        std::vector<std::string> datablockNames;
        std::vector<Ogre::HlmsPbsDatablock*> datablocks;
        // 与 datablocks 同下标：无贴图但带逐顶点颜色的基元走 Unlit，
        // 两者互斥，其中一个必为 nullptr。
        std::vector<Ogre::HlmsUnlitDatablock*> unlitDatablocks;
        std::vector<std::array<float, 4>> baseColours;
        std::vector<bool> alphaBlended;
        std::vector<PbrMaterialState> pbrMaterialStates;
        // 基元 → 上面那张去重表的下标。选中范围要按基元给色，但块是共享的，
        // 所以先由这个映射算出"哪些块被选中"，再按块着色。
        std::vector<std::uint32_t> primitiveDatablocks;
        std::uint32_t derivedPbrTextureCount = 0;
        std::uint64_t vertexCount = 0;
        std::uint64_t indexCount = 0;
        std::uint64_t textureCount = 0;
        std::uint32_t primitiveCount = 0;
        std::uint32_t selectedFirstPrimitive = 0;
        std::uint32_t selectedPrimitiveCount = 0;
        std::shared_ptr<const ModelMeshDto> sourceMesh;
        std::shared_ptr<const ModelMeshDto> pickMesh;
        Ogre::ManualObject* featureHighlightObject = nullptr;
        Ogre::HlmsUnlitDatablock* featureHighlightDatablock = nullptr;
        std::string featureHighlightDatablockName;
        std::uint32_t featureHighlightTriangleCount = 0;
        Ogre::ManualObject* edgeOverlayObject = nullptr;
        Ogre::HlmsUnlitDatablock* edgeOverlayDatablock = nullptr;
        std::string edgeOverlayDatablockName;
        bool pending = true;
        bool failed = false;
        bool tile = false;
        QString tileGroup;
        bool visible = true;
        bool userVisible = true;
        bool distanceLimited = false;
        float opacity = 1.0f;
        float groupOpacity = 1.0f;
        double minimumDistance = 0.0;
        double maximumDistance = 0.0;
        std::array<double, 3> offset {};
        std::array<double, 3> groupOffset {};
        // 瓦片组的水平刚体摆放（与 groupOffset 同源，随组设置继承）。
        double groupYawDegrees = 0.0;
        double groupHorizontalScale = 1.0;
        // 地理摆放（投影跟随算出的原点位置）：与用户通道的 groupOffset 分开存，
        // 切投影不会覆盖用户手动挪过的距离。
        std::array<double, 3> groupPlacement {};
        // 球面模式：contentOrigin − renderOrigin，随锚点每帧更新；平面模式恒零。
        // 锚定后投影跟随的 yaw/缩放/地理摆放不参与（朝向由内容原点决定）。
        bool globeAnchored = false;
        std::array<double, 3> globeBasePosition {};
        double placementYawDegrees() const noexcept
        {
            return globeAnchored ? 0.0 : groupYawDegrees;
        }
        double placementScale() const noexcept
        {
            return globeAnchored ? 1.0 : groupHorizontalScale;
        }
        std::array<float, 3> tint {1.0f, 1.0f, 1.0f};
        bool edgeLinesVisible = false;
        float brightness = 0.0f;
        float contrast = 1.0f;
        bool hasBounds = false;
        std::array<double, 3> boundsMinimum {};
        std::array<double, 3> boundsMaximum {};
    };

    // 上传前的纯 CPU 预处理结果：工作线程产出，渲染线程只负责提交到 GPU。
    struct PreparedTexture {
        std::int32_t sourceIndex = -1;
        std::uint8_t channel = 4u;
        std::uint16_t strengthKey = 1000u;
        bool srgb = false;
        bool valid = false;
        int width = 0;
        int height = 0;
        std::string cacheKey;
        std::vector<std::byte> rgba;
    };

    struct PreparedUpload {
        // 预处理时那份网格的身份：图层在排队期间换了几何（动态投影重放）时，
        // 旧的预处理结果必须作废，否则会把上一版顶点传上去。
        std::shared_ptr<const ModelMeshDto> sourceMesh;
        std::vector<PreparedTexture> textures;
        bool hasBounds = false;
        std::array<double, 3> boundsMinimum {};
        std::array<double, 3> boundsMaximum {};
        bool failed = false;
    };

    bool uploadLayer(const QString& layerKey, const PreparedUpload& prepared);
    bool addLayerInternal(const QString& layerKey,
                          std::shared_ptr<const ModelMeshDto> mesh, bool tile,
                          const QString& tileGroup);
    std::uint64_t totalRegisteredVertices() const noexcept;
    // 用户通道（组可见性/组透明度/组偏移）与 LOD 通道（每瓦片 visible/opacity/offset）叠加。
    bool effectiveVisible(const ModelResources& resources) noexcept;
    void applyNodeVisibility(ModelResources& resources);
    static float effectiveOpacity(const ModelResources& resources) noexcept;
    static std::array<double, 3> effectiveOffset(const ModelResources& resources) noexcept;
    // 摆放变换 = 平移(有效偏移) · 绕 +Y 右手旋转(groupYawDegrees) · 水平缩放(groupHorizontalScale)。
    // 节点的 setPosition/setOrientation/setScale 与包围盒/拾取的正反算共用同一约定，
    // 三者不一致就会出现「看得见但点不中」这类最难查的症状。
    static void placeLocalBounds(const std::array<double, 3>& localMinimum,
                                 const std::array<double, 3>& localMaximum,
                                 const ModelResources& resources, RenderBounds* out);
    static std::array<double, 3> worldToLocalPoint(const std::array<double, 3>& world,
                                                   const ModelResources& resources);
    static std::array<double, 3> localPointToWorld(const std::array<double, 3>& local,
                                                   const ModelResources& resources);
    void applyLayerOpacity(ModelResources& resources);
    void applyLayerPosition(ModelResources& resources);
    void applyLayerState(const QString& layerKey, ModelResources& resources);
    bool buildCurrentModel(const ModelMeshDto& mesh, const PreparedUpload& prepared);
    void clearCurrentModel();
    ModelResources takeCurrentModel();
    void installCurrentModel(ModelResources&& resources);
    Ogre::Vector3 layerDiffuse(const ModelResources& resources, std::size_t variant,
                               bool selected) const;
    // 选中范围落在哪些共享块上：按基元范围算，供按块着色使用。
    std::vector<bool> selectedVariants(const ModelResources& resources) const;
    bool createEdgeOverlay(const ModelMeshDto& mesh, Ogre::SceneNode* rootNode,
                           Ogre::ManualObject** object,
                           Ogre::HlmsUnlitDatablock** datablock,
                           std::string* datablockName);
    bool createPointOverlay(const ModelMeshDto& mesh, Ogre::SceneNode* rootNode,
                            Ogre::ManualObject** object,
                            Ogre::HlmsUnlitDatablock** datablock,
                            std::string* datablockName);
    void clearEdgeOverlay(Ogre::ManualObject*& object,
                          Ogre::HlmsUnlitDatablock*& datablock,
                          std::string& datablockName);
    bool applyDisplayMode(ModelResources& resources);
    void clearFeatureHighlight(ModelResources& resources);
    bool rebuildEnvironmentObjects();
    void clearEnvironmentObjects();
    bool createGlobeSurface();
    void clearGlobeSurface();
    void updateGlobeSurface();
    void emitGlobeGeometry();
    // 默认底图：<应用目录>/resources/globe_basemap.jpg（等距圆柱 2:1）。
    // 缺失或解码失败返回 nullptr，球面退回纯色。
    Ogre::TextureGpu* createGlobeBasemapTexture();
    void applyGlobeAnchorToLayers();
    bool createLightingResources();
    bool applyLightingStyle();
    void destroyLightingResources();
    void rebuildTileDebugOverlay();
    void clearTileDebugOverlay();
    void releaseTexture(Ogre::TextureGpu* texture);
    void notifyLayerState(const QString& layerKey, LayerRenderState state);
    void dispatchLayerState(const QString& layerKey, LayerRenderState state);

    struct TextureRequest {
        std::int32_t sourceIndex = -1;
        std::uint8_t channel = 4u;
        std::uint16_t strengthKey = 1000u;
        bool srgb = false;
    };
    void startPreprocessing(const QString& layerKey,
                            std::shared_ptr<const ModelMeshDto> mesh);
    void startPreprocessors();
    void stopPreprocessors();
    void preprocessWorker();
    static std::vector<TextureRequest> collectTextureRequests(const ModelMeshDto& mesh);
    static PreparedTexture preprocessTexture(const ModelTextureDto& source,
                                             const TextureRequest& request);
    static PreparedUpload preprocessMesh(const std::shared_ptr<const ModelMeshDto>& mesh);
    bool takePrepared(const QString& layerKey, PreparedUpload& out);
    void pruneOrphanPreparations();
    static void applyLayerViewMask(const ModelResources& resources, std::uint32_t viewMask);
    bool layerBounds(const QString& layerKey, RenderBounds* bounds) const;
    bool meshBounds(const ModelMeshDto& mesh,
                    const std::array<double, 3>& offset, RenderBounds* bounds) const;
    bool nodeBounds(const ModelMeshDto& mesh, std::uint32_t nodeIndex,
                    const std::array<double, 3>& offset, RenderBounds* bounds) const;

    OgreRuntime& m_runtime;
    Ogre::Root* m_root = nullptr;
    Ogre::SceneManager* m_sceneManager = nullptr;
    Ogre::Light* m_mainLight = nullptr;
    Ogre::SceneNode* m_mainLightNode = nullptr;
    bool m_ready = false;
    std::map<QString, ModelResources> m_layers;
    QQueue<QString> m_uploadQueue;
    struct TextureCacheEntry {
        Ogre::TextureGpu* texture = nullptr;
        std::uint32_t refs = 0;
    };
    std::function<void(const QString&, LayerRenderState)> m_layerStateCallback;
    // 存活标志：投递到 UI 线程的通知可能晚于场景析构到达。
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);

    struct PreprocessJob {
        QString layerKey;
        std::shared_ptr<const ModelMeshDto> mesh;
    };
    std::vector<std::thread> m_preprocessThreads;
    std::mutex m_preprocessMutex;
    std::condition_variable m_preprocessCondition;
    std::deque<PreprocessJob> m_preprocessJobs;
    bool m_preprocessStopping = false;
    std::mutex m_preparedMutex;
    std::map<QString, PreparedUpload> m_preparedUploads;
    std::map<std::string, TextureCacheEntry> m_textureCache;
    std::unordered_map<Ogre::TextureGpu*, std::string> m_textureKeys;

    // 瓦片组的用户通道状态；新注册的瓦片从这里继承当前组设置。
    struct TileGroupState {
        bool visible = true;
        float opacity = 1.0f;
        std::array<double, 3> offset {};
        // 显示投影切换后的整体跟随：绕 up 轴旋转（度，右手）+ 水平各向同性缩放。
        double yawDegrees = 0.0;
        double horizontalScale = 1.0;
        // 地理摆放：源局部原点在当前显示参照系下的场景局部坐标。
        std::array<double, 3> placement {};
    };
    std::map<QString, TileGroupState> m_tileGroups;

    std::vector<Ogre::ManualObject*> m_modelObjects;    std::vector<std::uint32_t> m_modelObjectNodeIndices;
    Ogre::SceneNode* m_modelNode = nullptr;
    std::vector<Ogre::SceneNode*> m_modelSceneNodes;
    std::vector<Ogre::TextureGpu*> m_modelTextures;
    std::vector<std::string> m_modelDatablockNames;
    std::vector<Ogre::HlmsPbsDatablock*> m_modelDatablocks;
    std::vector<Ogre::HlmsUnlitDatablock*> m_modelUnlitDatablocks;
    std::vector<std::array<float, 4>> m_modelBaseColours;
    std::vector<bool> m_modelAlphaBlended;
    std::vector<PbrMaterialState> m_modelPbrMaterialStates;
    std::vector<std::uint32_t> m_modelPrimitiveDatablocks;
    std::uint32_t m_modelDerivedPbrTextureCount = 0;
    Ogre::ManualObject* m_modelEdgeOverlayObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_modelEdgeOverlayDatablock = nullptr;
    std::string m_modelEdgeOverlayDatablockName;
    std::uint64_t m_uploadedVertexCount = 0;
    std::uint64_t m_uploadedIndexCount = 0;
    std::uint64_t m_uploadedTextureCount = 0;
    std::uint32_t m_uploadedPrimitiveCount = 0;
    std::shared_ptr<const ModelMeshDto> m_modelPickMesh;
    std::uint32_t m_selectedFirstPrimitive = 0;
    std::uint32_t m_selectedPrimitiveCount = 0;
    bool m_modelVisible = true;
    bool m_modelHasBounds = false;
    std::array<double, 3> m_modelBoundsMinimum {};
    std::array<double, 3> m_modelBoundsMaximum {};

    ViewportEnvironmentStyle m_environmentStyle;
    ViewportLightingStyle m_lightingStyle;
    ViewportDisplayMode m_displayMode = ViewportDisplayMode::Solid;
    FeatureHighlightStyle m_featureHighlightStyle;
    std::uint64_t m_renderBudgetMaxVertices = 50000000u;
    std::uint32_t m_distanceLimitedCount = 0;
    // 上传链路计时：预处理在工作线程，提交在渲染线程且受每帧预算限制。
    // 15 块/秒的加载速率到底是读盘慢还是提交慢，只能靠这两组计数判定。
    std::atomic<std::uint64_t> m_preprocessCount { 0 };
    std::atomic<std::uint64_t> m_preprocessUs { 0 };
    std::uint64_t m_uploadCount = 0;
    std::uint64_t m_uploadUs = 0;
    std::uint64_t m_uploadFrames = 0;
    std::uint64_t m_uploadBudgetHits = 0;
    struct ViewSlot {
        Ogre::Camera* camera = nullptr;
    };
    std::map<std::uint32_t, ViewSlot> m_viewSlots;
    Ogre::ManualObject* m_gridObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_gridDatablock = nullptr;
    std::string m_gridDatablockName;
    Ogre::ManualObject* m_axesObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_axesDatablock = nullptr;
    std::string m_axesDatablockName;
    bool m_axesPlanar = false;
    Ogre::ManualObject* m_tileDebugObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_tileDebugDatablock = nullptr;
    std::string m_tileDebugDatablockName;
    bool m_tileDebugEnabled = false;

    // VIEW3D_PBS_REFLECTION=1 时的天空反射探针；未开启时为空。
    std::shared_ptr<PbsReflection> m_pbsReflection;

    // 球面模式：渲染原点跟随活动视口相机（ECEF），瓦片按 contentOrigin − 原点
    // 摆放；椭球底面顶点同帧重定心。关闭时这些资源全部销毁，每帧路径零开销。
    bool m_globeMode = false;
    bool m_globeAnchorValid = false;
    std::array<double, 3> m_globeRenderOrigin {};
    Ogre::ManualObject* m_globeObject = nullptr;
    Ogre::HlmsUnlitDatablock* m_globeDatablock = nullptr;
    Ogre::TextureGpu* m_globeBasemapTexture = nullptr;
    std::string m_globeDatablockName;
    std::vector<double> m_globeVerticesEcef;
    std::vector<std::uint32_t> m_globeIndices;
    // 运行期替换的球面底图（在线影像）：空表示用内置静态图。
    std::vector<unsigned char> m_globeCustomBasemapRgba;
    int m_globeCustomBasemapWidth = 0;
    int m_globeCustomBasemapHeight = 0;
    std::map<std::uint64_t, std::function<void(bool)>> m_globeListeners;
    std::uint64_t m_globeListenerToken = 0;
};

} // namespace View3D
