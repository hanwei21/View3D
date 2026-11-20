#pragma once

#include "OsgbGeometry.h"
#include "OsgbMetadata.h"

#include <TileCacheSettings.h>

#include <QHash>
#include <QString>
#include <QTimer>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace View3D {
class IGeoReference;
class IRenderScene;
class IViewportLayout;
struct ViewportCameraState;
} // namespace View3D

namespace View3DOsb {

// 倾斜摄影瓦片流的调度与缓存。三层结构：
//   L0 索引：LOD 骨架（中心/半径/子文件路径），常驻但有上限，超出按 LRU 遗忘；
//            被遗忘的瓦片可以由父级索引重新发现，因此遗忘不丢可达性。
//   L1 内容：工作线程解码出的 CPU 网格与纹理。
//   L2 显存：上传到场景的几何与贴图（按含 mip 的估算值一起记账）。
// 淘汰用两条侵入式 LRU 链：先丢"下载完但从未上屏"的，再丢"最久没上屏"的；
// 显示中、被需求（子瓦片未就位时的替补）、在飞的瓦片受保护。
// 容量分两级：cacheBytes 软上限触发淘汰；cacheBytes+overflowBytes 硬上限
// 额外强制放大屏幕空间误差，宁可退回更粗层级也不无界增长。
class OsgbStreaming {
  public:
    OsgbStreaming(View3D::IRenderScene& scene, View3D::IViewportLayout* layout,
                  DatasetDescriptor descriptor, View3D::TileCacheSettings settings = {},
                  View3D::IGeoReference* reference = nullptr);
    ~OsgbStreaming();

    OsgbStreaming(const OsgbStreaming&) = delete;
    OsgbStreaming& operator=(const OsgbStreaming&) = delete;

    // 球面模式：烘焙路径在会话创建时定死，切换只能重建会话（与 3D Tiles 一致）。
    // 必须在 start() 前调用。
    void setGlobeMode(bool enabled) noexcept { m_globeMode = enabled; }
    bool globeMode() const noexcept { return m_globeMode; }
    // 模式切换重建会话时保留当前相机：跳过数据集自动框取景。
    void keepCameraOnRestart() noexcept { m_framed = true; }

    // 加载根瓦片并启动周期性 LOD 更新；返回是否至少注册了一块瓦片。
    bool start();
    void stop();
    // start() 失败时带上第一块读不出来的根瓦片原因（换机器排查靠它定位路径/数据问题）。
    QString lastError() const { return m_lastError; }

    std::uint32_t residentTiles() const noexcept { return m_residentTiles; }
    std::uint32_t shownTiles() const noexcept { return m_shownTiles; }
    std::uint32_t deepestLevel() const noexcept { return m_deepestLevel; }
    // 常驻内容的 CPU + 显存估算字节数（缓存策略的记账口径）。
    std::uint64_t retainedBytes() const noexcept { return m_retainedBytes; }
    std::uint64_t indexBytes() const noexcept { return m_indexBytes; }
    std::uint64_t loadedTiles() const noexcept { return m_loadedTotal; }
    std::uint64_t reloadedTiles() const noexcept { return m_reloadTotal; }
    std::uint64_t evictedTiles() const noexcept { return m_evictTotal; }
    // 首次收敛（所有需求都已就位）所花的时间，用于量化初次加载速度；未收敛为负。
    double steadyMilliseconds() const noexcept { return m_steadyMs; }
    const QString& groupKey() const noexcept { return m_groupKey; }
    const View3D::TileCacheSettings& settings() const noexcept { return m_settings; }
    // start() 后实际减掉的数据集原点（metadata 的 SRSOrigin，或 0 表示成果已是局部坐标）：
    // 图层属性展示的"源→局部"矩阵要与它保持一致。
    const std::array<double, 3>& appliedOrigin() const noexcept { return m_origin; }
    // 把场景中该组的瓦片全部撤下，但保留索引与线程，便于再次显示。
    void detachFromScene();

  private:
    // 队列优先级：中心锥内 > 视野内 > 兄弟预取。
    enum QueueTier { TierCenter = 0, TierView = 1, TierPrefetch = 2 };
    static constexpr int kTierCount = 3;
    // 递归深度上限：正常 LOD 树不超过十几层，超限只可能是数据索引异常，直接截断。
    static constexpr int kMaxLodDepth = 32;
    // LRU 链编号。
    enum LruList { LruRendered = 0, LruUpdated = 1, kLruCount = 2 };

    struct LodSlot {
        double minimum = 0.0;
        double maximum = 0.0;
        QString childFile;
        QString childKey;
        bool external = false;
    };

    struct LodNode {
        std::array<double, 3> center {};
        double radius = 0.0;
        // 同一文件内嵌套 PagedLOD 的父节点序号；-1 表示顶层。
        int parentIndex = -1;
        // rangeSlots 的 min/max 单位：真为"包围球投影像素尺寸"，假为"到球心距离（米）"。
        bool pixelSizeMode = false;
        // 不能命名为 slots：Qt 把 slots 定义为宏。
        std::vector<LodSlot> rangeSlots;
    };

    // 一个数据集需要的相机信息：位置、朝向、视锥与像素当量。
    // 视锥用相机基 + 单侧平面的正余弦表示，球-视锥相交测试与屏幕位置无关。
    struct CameraFrame {
        std::array<double, 3> position {};
        std::array<double, 3> direction {1.0, 0.0, 0.0};
        std::array<double, 3> right {0.0, 0.0, 1.0};
        std::array<double, 3> up {0.0, 1.0, 0.0};
        // 放宽 0.12 rad 后的半角，四侧面法线 = 基向量×cos − 视线×sin。
        double cosHorizontal = 0.9238795;
        double sinHorizontal = 0.3826834;
        double cosVertical = 0.9238795;
        double sinVertical = 0.3826834;
        double centerCosine = 1.0;
        double pixelsPerRadian = 0.0;
        double pixelsPerUnit = 0.0;
        // Cesium dynamicScreenSpaceError 的密度：dseDensity × 俯仰系数 × 高度系数。
        double dseDensity = 0.0;
        bool orthographic = false;
    };

    // 单个 PagedLOD 节点在本轮的几何评估。两个遍历共用，避免两份公式漂移。
    struct NodeView {
        bool inView = false;
        bool refine = false;
        double toCentre = 0.0;
        // 到包围球最近点的距离：Cesium 计算 SSE 用的就是它。
        double distance = 0.0;
        // 投影半径减去 dynamicScreenSpaceError 后的等效误差（px）。
        double error = 0.0;
        double cosAngle = 1.0;
        // 本距离命中的全部外部槽位（bit i ↔ rangeSlots[i]）。OSG 的 PagedLOD
        // 可以同时显示多个子节点，不能只取第一个命中项。
        std::uint64_t externalSlots = 0;
        // 命中了内嵌槽位（没有外部文件）：本文件的自身几何必须保留。
        bool hasInlineSlot = false;
    };

    struct Tile {
        QString key;
        QString path;
        int level = 0;
        bool resident = false;
        // 场景层已经完成 GPU 上传、真正可显示。resident 只代表 CPU 内容就位，
        // 不能拿它当"可替换父级"的依据，否则父级先隐藏、子级几帧后才出现。
        bool renderReady = false;
        // 登记到场景且自身有几何：空文件（只有 LOD 索引）不占场景图层，也无从显示。
        bool displayable = false;
        bool loading = false;
        bool failed = false;
        bool demanded = false;
        bool prefetch = false;
        bool inCenterCone = false;
        // 上一次真正下发给场景的可见性；登记时场景默认瓦片可见，所以初值为真。
        bool shown = false;
        // 上一次下发的节点可见掩码：bit i ↔ 第 i 个 PagedLOD 组的自身几何。
        std::uint64_t shownMask = 0;
        // 文件里存在不属于任何 PagedLOD 的几何：它跟随图层可见性，不可被子级替换。
        bool hasRootGeometry = false;
        // 每轮按最小值取需求距离，新登记的瓦片必须从正无穷开始参与比较。
        double demandDistance = std::numeric_limits<double>::max();
        double demandAngle = std::numeric_limits<double>::max();
        // 超出误差阈值的倍数：越大越该先加载（近处超得多，远处刚过线）。
        double demandUrgency = 0.0;
        qint64 retryAtMs = 0;
        // 被淘汰后的冷却：期内不允许再作为"预取"拉回来。预取的瓦片从未上屏、
        // 是第一批被淘汰的，不冷却就会形成预取→淘汰→再预取的无限循环。
        qint64 cooldownUntilMs = 0;
        int failureStreak = 0;
        // L0 索引（骨架）最近一次被遍历到的时间：只给 forgetIndexes 的 LRU 用。
        qint64 lastIndexMs = 0;
        // 内容最近一次上屏的时间：只给内容淘汰的宽限期用。
        // 这两个必须是分开的字段：早先合成一个 lastUsedMs 时，propagate 每轮都会刷新
        // 整条遍历链，于是"常驻但不显示"的祖先永远落在 keepAliveMs 宽限期内，
        // 淘汰根本选不到它们——实测 resident=300 而 shown 只有 47。
        qint64 lastContentMs = 0;
        std::size_t bytes = 0;
        std::size_t indexBytes = 0;
        // 每轮至多展开一次：共享子瓦片不重复遍历，索引成环也不会递归失控。
        std::uint64_t demandPass = 0;
        std::vector<LodNode> nodes;
        // 侵入式 LRU 句柄；lruList 为 -1 表示不在任何链上。
        Tile* lruPrev = nullptr;
        Tile* lruNext = nullptr;
        int lruList = -1;
    };

    struct Job {
        QString key;
        QString path;
        double angle = 0.0;
        double distance = 0.0;
        int level = 0;
    };

    struct LruHead {
        Tile* head = nullptr;
        Tile* tail = nullptr;
        std::uint32_t count = 0;
    };

    // 本轮要下发的显示项：图层可见（presence）+ 各 PagedLOD 组自身几何的掩码。
    struct ShownEntry {
        Tile* tile = nullptr;
        std::uint64_t nodeMask = 0;
    };

    static std::uint64_t nodeMaskAll(std::size_t count)
    {
        if (count == 0)
            return 0;
        return count >= 64 ? ~0ull : ((1ull << count) - 1ull);
    }

    void tick();
    // 两遍遍历，职责分开：
    //   propagate —— 只决定"该下载谁"，按索引自由下钻，内容被淘汰也不断链；
    //   selectShown —— 只决定"该显示谁"，父级顶上前会把自己子树的显示贡献整份丢弃，
    //                  因此结构上不可能出现父子同时显示。
    // 早先两者合成一遍时，"父级仍需顶上"与"子级已可显示"会同时成立并都进显示集，
    // 表现为粗父瓦片的模糊地皮盖在精细建筑上。
    void propagate(Tile& tile, const CameraFrame& camera, int depth);
    bool selectShown(Tile& tile, const CameraFrame& camera, int depth,
                     std::vector<ShownEntry>& shownOut);
    NodeView evaluateNode(const LodNode& node, const CameraFrame& camera, double threshold) const;
    bool pickCamera(CameraFrame* camera) const;
    Tile& ensureTile(const QString& key, const QString& path, int level);
    Tile* tileAt(const QString& key) noexcept;
    // 瓦片键：必须是"路径的纯函数"，并且不同文件之间不撞。
    QString tileKeyFor(const QString& path) const;
    void requestLoads();
    void dispatchTier();
    void evictToBudget();
    void updateRefineThreshold();
    void refreshRenderReadiness();
    void forgetIndexes();
    void noteFailure(Tile& tile, const QString& reason);
    void applyVisibility();
    void updateSteady();
    void frameOnce();
    void logProgress();
    void lruUnlink(Tile& tile);
    void lruPushFront(int list, Tile& tile);
    void lruTouch(Tile& tile);
    void workerMain();
    void adoptConverted(const QString& key, ConvertedTile&& tile);
    int inFlightCount() const;
    std::size_t estimatedIndexBytes(const ConvertedTile& converted) const;
    // 球面烘焙（工作线程）：逐顶点 源网格坐标 → 经纬度 → ECEF；无源参照系的
    // 成果把局部原点钉在 (0°,0°) 地表按 ENU 摆放。顶点改为相对本瓦片 ECEF
    // 原点的偏移，法线旋转到 ECEF 轴。失败时 tile.error 给出原因。
    bool bakeTileToGlobe(ConvertedTile& tile) const;
    // 数据集原点（SRSOrigin 的经纬度；无参照系时取 (0°,0°)）及其 ENU 基与 ECEF。
    void resolveGlobeOrigin();
    // 球面相机（ECEF）→ 数据集局部系，LOD 遍历继续用本地包围球公式。
    void globeCameraToLocal(View3D::ViewportCameraState* state) const;

    View3D::IRenderScene& m_scene;
    View3D::IViewportLayout* m_layout;
    DatasetDescriptor m_descriptor;
    View3D::TileCacheSettings m_settings;
    View3D::IGeoReference* m_reference = nullptr;
    QString m_groupKey;
    std::array<double, 3> m_origin {};
    // 球面模式：数据集锚点（SRSOrigin 的经纬度，无参照系取 0,0）与 ENU 基。
    bool m_globeMode = false;
    bool m_globeOriginResolved = false;
    std::array<double, 3> m_globeOriginGeographic { {0.0, 0.0, 0.0} };
    std::array<double, 3> m_globeOriginEcef {};
    std::array<double, 3> m_globeEast { {0.0, 1.0, 0.0} };
    std::array<double, 3> m_globeNorth { {0.0, 0.0, 1.0} };
    std::array<double, 3> m_globeUp { {1.0, 0.0, 0.0} };

    // 瓦片对象放堆上且按 key 索引：std::map 允许 value_type 是 move-only，
    // 同时保证引用地址稳定（QHash 会要求可拷贝）。
    std::map<QString, std::unique_ptr<Tile>> m_tiles;
    QStringList m_rootKeys;
    // 本轮显示集：key 存在即图层可见，值是该文件内各 PagedLOD 组自身几何的掩码。
    QHash<QString, std::uint64_t> m_showThisPass;
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);

    QTimer m_tick;
    QObject m_context;
    QString m_lastError;
    std::uint32_t m_residentTiles = 0;
    std::uint32_t m_shownTiles = 0;
    std::uint32_t m_deepestLevel = 0;
    std::uint32_t m_pendingDemand = 0;
    std::uint64_t m_retainedBytes = 0;
    std::uint64_t m_indexBytes = 0;
    std::uint64_t m_loadedTotal = 0;
    std::uint64_t m_reloadTotal = 0;
    std::uint64_t m_evictTotal = 0;
    std::uint64_t m_failedTotal = 0;
    std::uint64_t m_fallbackPasses = 0;
    // 被两个父级同时引用的子瓦片次数：倾斜摄影的 LOD 图不一定是严格四叉树。
    std::uint64_t m_sharedChildEdges = 0;
    // 本轮被更细子瓦片替换掉的 PagedLOD 组数：替换真的发生在节点粒度上的证据。
    std::uint64_t m_replacedGroups = 0;
    // 本轮可见切换的层级跳变量：jump = 新显示的最深层级 − 父级文件层级。
    std::uint64_t m_switchCount = 0;
    std::uint64_t m_switchJumpSum = 0;
    int m_switchJumpMax = 0;
    // 本轮从"已被子级替换"退回"显示父级自身几何"的节点数：来回翻的证据。
    std::uint64_t m_revertCount = 0;
    std::uint64_t m_hitTiles = 0;
    std::uint64_t m_missTiles = 0;
    qint64 m_lastLogMs = 0;
    qint64 m_startMs = 0;
    std::uint64_t m_passSerial = 0;
    qint64 m_firstShownMs = 0;
    bool m_framed = false;
    double m_steadyMs = -1.0;
    double m_readMs = 0.0;
    double m_convertMs = 0.0;
    // 实际生效的误差阈值 = 配置值在内存压力下按 Cesium 的
    // memoryAdjustedScreenSpaceError 方式缓慢放大（每拍 4%，25 Hz 下约等于
    // Cesium 60 Hz 的 2%），低于 cacheBytes 时再缓慢回落。需求集因此整屏一起
    // 变粗/变细，不会出现"左边细右边粗"的局部饿死。
    double m_refinePixels = 16.0;
    // 本轮加载闸门是否因预算/块数上限拒绝了仍被需求的瓦片。
    bool m_budgetBlocked = false;
    // 本轮已登记但 GPU 尚未上传完成的瓦片数（诊断切换生硬/闪空的来源）。
    std::uint32_t m_uploadPending = 0;
    // 诊断开关（VIEW3D_OSBG_LEGACY_READY=1）：跳过渲染就绪闸门与上传背压，
    // 复现旧行为，用于 A/B 对比切换生硬/闪空。
    bool m_legacyReady = false;
    // 本轮实际使用的 DSE 密度，只用于日志里换算 dse800。
    double m_dseDensity = 0.0;
    // 相机相对数据集最低点的高度，每轮刷新；只用于日志与 DSE 的高度系数。
    double m_cameraHeightAboveGround = 0.0;
    // 相机高出包围球心的量，只用于打印。
    double m_cameraHeightAboveCenter = 0.0;
    // 诊断开关（VIEW3D_OSBG_KEEP_PARENTS=1）：父级永不退场，父子叠加显示。
    // 用来判定一个前提是否成立——3D Tiles 规范保证"子级完全覆盖父级"，
    // 倾斜摄影没有这条保证。若打开后空白消失而 overlap 上升，说明空白来自
    // 父级独有的、没有子级接替的那部分地面，REPLACE 语义对这份数据不成立。
    bool m_keepParents = false;
    // A/B 开关（VIEW3D_OSBG_METER_SLOTS=1）：无视成果声明的 RangeMode，一律按
    // "到球心距离（米）"比较距离槽，用来复现修正前的口径并量清晰度差值。
    bool m_meterSlotsOnly = false;
    // 只有缓存确实还有空位时才允许兄弟预取：预取的瓦片从未上屏、是第一批被淘汰的，
    // 缓存已满时它会形成"预取→淘汰→再预取"的死循环（实测 95% 加载都是这种无效功）。
    bool m_prefetchAllowed = true;
    bool m_hardOverflow = false;
    bool m_hasBounds = false;
    std::array<double, 3> m_boundsMinimum {};
    std::array<double, 3> m_boundsMaximum {};
    View3DOsb::TileCounts m_counts;

    LruHead m_lru[kLruCount];
    std::vector<std::thread> m_workers;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCondition;
    std::deque<Job> m_queues[kTierCount];
    bool m_stopping = false;
};

} // namespace View3DOsb
