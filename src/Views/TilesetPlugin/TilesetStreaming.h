#pragma once

#include <TileCacheSettings.h>

#include <QString>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace View3D {
class IGeoReference;
class IRenderScene;
class IViewportLayout;
}

namespace View3DTiles {

// 所有 3D Tiles 数据集共用同一个局部坐标系：原点取第一个可用包围体的 ECEF
// 中心，局部轴为 ENU，再按 View3D 的 Y-up 约定 (E,U,-N) 旋转。多个数据集
// 因此仍保持彼此正确的相对位置；相机也按同一变换换到 ECEF 交给 Cesium。
class TilesetWorldFrame final {
  public:
    struct Snapshot {
        bool ready = false;
        glm::dvec3 originEcef { 0.0 };
        glm::dmat4 ecefToLocal { 1.0 };
        glm::dmat4 localToEcef { 1.0 };
    };

    // 线程安全；只接受第一次调用。返回是否已就绪。
    bool ensureInitialized(const glm::dvec3& originEcef);
    // 与 plugin.view3d.crs 共享世界原点：先领用已钉住的 ECEF 原点，没有则用第一个
    // 候选点把 crs 钉住。这样 3D Tiles 与影像/二维矢量叠加时不会互相错位。
    void setSharedReference(View3D::IGeoReference* reference);
    bool ready() const;
    Snapshot snapshot() const;

  private:
    mutable std::mutex m_mutex;
    bool m_ready = false;
    glm::dvec3 m_originEcef { 0.0 };
    glm::dmat4 m_ecefToLocal { 1.0 };
    glm::dmat4 m_localToEcef { 1.0 };
    View3D::IGeoReference* m_sharedReference = nullptr;
};

using TilesetWorldFramePtr = std::shared_ptr<TilesetWorldFrame>;

// 一个 tileset.json 的流式加载会话：Cesium Native 负责 LOD 遍历、内容缓存
// 与异步加载，本类负责把选中的瓦片同步到 View3D 场景的瓦片组。
class TilesetStreaming final {
  public:
    TilesetStreaming(View3D::IRenderScene& scene, View3D::IViewportLayout* layout,
                     TilesetWorldFramePtr frame, QString tilesetPath, QString groupKey,
                     QString displayName, View3D::TileCacheSettings settings);
    ~TilesetStreaming();

    TilesetStreaming(const TilesetStreaming&) = delete;
    TilesetStreaming& operator=(const TilesetStreaming&) = delete;

    bool start();
    void stop();
    // 只撤场景中的瓦片，保留 Tileset 与缓存，便于再次显示。
    void detachFromScene();

    // 球面模式：内容烘焙到「内容原点系」，ECEF 原点记入 DTO，渲染端减锚点摆放。
    // 必须在 start 之前设置；宿主切换模式时按 stop→set→start 重启会话。
    void setGlobeMode(bool enabled);
    bool globeMode() const noexcept;
    // 模式切换重启后保留宿主已换算好的相机，不再按根包围体重新取景。
    void keepCameraOnRestart();

    // 世界框架算出来时回调一次（主线程）：图层属性要显示源→局部的转换矩阵。
    using WorldFrameCallback = std::function<void(const TilesetWorldFrame::Snapshot&)>;
    void setWorldFrameCallback(WorldFrameCallback callback);

    const QString& groupKey() const noexcept { return m_groupKey; }
    const QString& tilesetPath() const noexcept { return m_tilesetPath; }
    const QString& displayName() const noexcept { return m_displayName; }

    std::uint32_t residentTiles() const noexcept;
    std::uint32_t visibleTiles() const noexcept;
    std::uint64_t loadedTiles() const noexcept;
    std::uint64_t requests() const noexcept;
    bool rooted() const noexcept;
    QString lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    QString m_groupKey;
    QString m_tilesetPath;
    QString m_displayName;
};

} // namespace View3DTiles
