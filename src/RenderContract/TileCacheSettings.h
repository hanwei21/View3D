#pragma once

#include <QString>

#include <algorithm>
#include <cstdint>

namespace View3D {

// 流式瓦片（倾斜摄影等）的缓存策略参数。结构内部一律按字节记账，
// 配置文件里用 MB 书写，解析时换算并做合法区间收敛。
struct TileCacheSettings {
    // 软上限：常驻内容超过它就按 LRU 淘汰。
    std::uint64_t cacheBytes = 1024ull << 20;
    // 硬上限余量：常驻超过 cacheBytes + overflowBytes 时强制放大屏幕空间误差，
    // 宁可退回更粗层级，也不让内存与显存无界增长。
    std::uint64_t overflowBytes = 256ull << 20;
    // L0 索引层（LOD 骨架，不含几何）的上限；超出就遗忘最久未用的骨架，
    // 被遗忘的瓦片仍可由父级索引重新发现。
    std::uint64_t indexCacheBytes = 40ull << 20;
    // 常驻块数上限，与字节上限共同生效（小瓦片很多时按块数兜底）。
    std::uint32_t maxTiles = 1200u;
    // 基础屏幕空间误差：节点在画面上的投影半径不足这么多像素就不再向下钻取。
    double screenSpaceErrorPixels = 16.0;
    // 中心优先锥的半角系数：视野中心这个比例的锥内需求进最高优先级队列。
    double centerConeFactor = 0.6;
    // 读取与转换的工作线程数。
    int workers = 6;
    // 在飞任务上限，超过就不再派发新任务（反压）。按实测取值：单块读+转约 5.5 ms，
    // 6 个线程要喂满需要 30 以上在飞；上限太小会让线程空转（看着就像单线程）。
    int maxInFlight = 32;
    // 已登记但 GPU 尚未上传完成的瓦片上限。CPU 转换远快于每帧 8 ms 的上传预算，
    // 不设上限会积压上百块、最后一波集中替换父级，画面就会成片跳变。
    std::uint32_t maxUploadPending = 96u;
    // 淘汰宽限：这段时间内上过屏的瓦片不会被选为淘汰对象，抑制边界抖动。
    int keepAliveMs = 2000;
    // 是否以最低优先级预取兄弟瓦片。Cesium 的 loadSiblings 默认 false：
    // 预取的瓦片从未上屏、是第一批被淘汰的，缓存紧张时会形成预取→淘汰→再预取的循环。
    bool preloadSiblings = false;
    // 距离自适应误差（Cesium dynamicScreenSpaceError）：
    // 误差减去 (1 - exp(-(distance×density)^2)) × factor，远处放宽、近处保持严格。
    bool dynamicScreenSpaceError = true;
    double dseFactor = 24.0;
    // 密度基准（Cesium 默认 0.00278），实际密度再乘视线俯仰与相机高度得到的系数。
    double dseDensity = 0.00278;
    // 密度开始衰减的高度占比（Cesium 推荐 0.25）。
    double dseHeightFalloff = 0.25;
    QString preset = QStringLiteral("balanced");

    // 预设档位：low / balanced / high。未知名字按 balanced 处理。
    // 口径是"CPU 保留 + 显存副本"的合计字节数，因此数值比只算 CPU 时大一倍左右。
    static TileCacheSettings forPreset(const QString& name)
    {
        TileCacheSettings settings;
        if (name == QLatin1String("low")) {
            settings.cacheBytes = 512ull << 20;
            settings.overflowBytes = 128ull << 20;
            settings.indexCacheBytes = 20ull << 20;
            settings.maxTiles = 600u;
            settings.screenSpaceErrorPixels = 24.0;
            settings.workers = 3;
            settings.maxInFlight = 12;
            settings.preloadSiblings = false;
        } else if (name == QLatin1String("high")) {
            settings.cacheBytes = 2048ull << 20;
            settings.overflowBytes = 512ull << 20;
            settings.indexCacheBytes = 80ull << 20;
            settings.maxTiles = 2600u;
            settings.screenSpaceErrorPixels = 12.0;
            settings.workers = 8;
            settings.maxInFlight = 48;
        } else {
            settings.cacheBytes = 1024ull << 20;
            settings.overflowBytes = 256ull << 20;
            settings.indexCacheBytes = 40ull << 20;
            settings.maxTiles = 1200u;
            settings.screenSpaceErrorPixels = 16.0;
            settings.workers = 6;
            settings.maxInFlight = 32;
        }
        settings.preset = name;
        settings.clampToValidRange();
        return settings;
    }

    // 逐项夹到合法区间：配置写错时宁可退化到保守值，也不能停摆或无界增长。
    void clampToValidRange()
    {
        cacheBytes = std::clamp<std::uint64_t>(cacheBytes, 64ull << 20, 8ull << 30);
        overflowBytes = std::clamp<std::uint64_t>(overflowBytes, 32ull << 20, 4ull << 30);
        indexCacheBytes = std::clamp<std::uint64_t>(indexCacheBytes, 256ull << 10, 512ull << 20);
        maxTiles = std::clamp<std::uint32_t>(maxTiles, 64u, 20000u);
        screenSpaceErrorPixels = std::clamp(screenSpaceErrorPixels, 2.0, 128.0);
        centerConeFactor = std::clamp(centerConeFactor, 0.05, 1.0);
        workers = std::clamp(workers, 1, 16);
        maxInFlight = std::max(workers, std::clamp(maxInFlight, 1, 64));
        maxUploadPending = std::clamp<std::uint32_t>(maxUploadPending, 8u, 2048u);
        keepAliveMs = std::clamp(keepAliveMs, 0, 30000);
        dseFactor = std::clamp(dseFactor, 0.0, 64.0);
        dseDensity = std::clamp(dseDensity, 0.0, 0.05);
        dseHeightFalloff = std::clamp(dseHeightFalloff, 0.01, 1.0);
    }
};

// 配置宿主：由持有 system_config.xml 的模块发布，流式插件只读消费。
class ITileCacheConfig {
  public:
    virtual ~ITileCacheConfig() = default;
    virtual TileCacheSettings tileCacheSettings() const = 0;
};

} // namespace View3D
