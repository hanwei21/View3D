#pragma once

#include "CrsBackend.h"

#include <IGeoReference.h>

#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace View3DCrs {

// 主坐标系实现：WGS84 经纬度+椭球高 ↔ 场景局部米制坐标（Y-up 的 (E,U,-N)）。
// 局部轴的取向由「显示参照系」决定：默认（主参照系）取世界原点处的 ENU 切平面，
// 真北朝上；设为投影系（EPSG:3857、UTM、CGCS2000 高斯-克吕格…）时局部轴改成该
// 投影的网格轴，网格北朝上、单位是网格米——逐点位置走 PROJ 精确变换，法线与瓦片
// 这类整体摆放用原点处的平面微分（收敛角 + 比例因子）做线性跟随。
// 世界原点在第一个地理数据源到达时钉住，之后只有显式 resetOrigin 才会移动，
// 切换显示参照系不搬原点，只改取向，因此多源叠加仍然同源。
class CrsReference final : public View3D::IGeoReference {
  public:
    CrsReference();

    bool ready() const noexcept override;
    bool setOrigin(double lonDeg, double latDeg, double heightMeters) override;
    void resetOrigin() override;
    bool setOriginFromEcef(const std::array<double, 3>& ecefMeters) override;
    bool lonLatHeightToLocal(double lonDeg, double latDeg, double heightMeters,
                             std::array<double, 3>* localXyz) const override;
    bool localToLonLatHeight(const std::array<double, 3>& localXyz, double* lonDeg,
                             double* latDeg, double* heightMeters) const override;
    QString horizontalCrs() const override;
    QString mainCrs() const override;
    bool origin(double* lonDeg, double* latDeg, double* heightMeters) const override;
    bool originEcef(std::array<double, 3>* ecefMeters) const override;
    bool localToEcefPoint(const std::array<double, 3>& localXyz,
                          std::array<double, 3>* ecefMeters) const override;
    bool ecefToLocalPoint(const std::array<double, 3>& ecefMeters,
                          std::array<double, 3>* localXyz) const override;
    bool crsInfo(const QString& sourceCrs, CrsInfo* info) const override;
    bool transformToMain(const QString& sourceCrs, double* xyz,
                         std::size_t pointCount) const override;

    QString displayCrs() const override;
    bool dynamicProjectionEnabled() const override;
    bool setDynamicProjectionEnabled(bool enabled) override;
    bool setDisplayCrs(const QString& crs) override;
    bool planeFrameAt(const QString& crs, double lonDeg, double latDeg, double heightMeters,
                      PlaneFrame* frame) const override;
    bool planePlacement(const QString& sourceCrs, double sourceOriginLonDeg,
                        double sourceOriginLatDeg, double sourceOriginHeightMeters,
                        PlanePlacement* placement) const override;
    bool sourcePointsToLocal(const QString& sourceCrs, const double* xyz,
                             std::size_t pointCount, std::vector<double>* localXyz) const override;
    std::uint64_t addGeoFrameChangeListener(
        std::function<void(GeoFrameChange change, const QString& displayCrs)> callback) override;
    void removeGeoFrameChangeListener(std::uint64_t token) override;

    // 上一次 setDisplayCrs / 变换失败的原因，供 UI 与日志直接引用。
    QString lastError() const override;

    // 供影像服务查询后端是否具备真正的重投影能力（无 PROJ 时只有 4326/3857）。
    const CrsBackend& backend() const noexcept { return m_backend; }
    void setProjDataDirectory(const QString& directory);

  private:
    struct Frame {
        double lonDeg = 0.0;
        double latDeg = 0.0;
        double heightMeters = 0.0;
        std::array<double, 3> ecef { {0.0, 0.0, 0.0} };
        // 原点处的 ENU 基向量（ECEF 表示），用于 ECEF↔局部 的线性部分。
        double sinLat = 0.0;
        double cosLat = 1.0;
        double sinLon = 0.0;
        double cosLon = 1.0;
    };

    struct Display {
        QString crs;   // 交给后端的参照系文本；空表示 ENU 切平面显示
        bool projected = false;   // 上面这个 crs 是不是投影系（有自己的网格轴）
        // 世界原点在投影面上的 (东,北) 米：局部平面坐标 = 投影坐标 − 该值。
        std::array<double, 2> originGrid { {0.0, 0.0} };
        PlaneFrame frame;   // 原点处 真(东,北) -> 网格(东,北) 的微分
        bool valid = false;
    };

    // 以下 *Locked 私有方法要求调用方已持有 m_mutex；m_mutex 不可重入。
    void enuOf(double lonDeg, double latDeg, double heightMeters,
               std::array<double, 3>* eastNorthUp) const;
    bool frameOf(const QString& crs, double lonDeg, double latDeg, double heightMeters,
                 PlaneFrame* frame) const;
    void refreshDisplayLocked();
    // 动态投影开关关着时，即便记着某个投影系也不生效：局部轴回到真北 ENU 切平面。
    bool projectionActiveLocked() const { return m_projectionEnabled && m_display.projected; }
    // 回调要在锁外调用（监听者往往会回头查 displayCrs/origin），所以先取快照。
    std::vector<std::function<void(GeoFrameChange, const QString&)>> listenersLocked() const;
    QString displayCrsTextLocked() const;

    using GeoFrameCallback =
        std::function<void(View3D::IGeoReference::GeoFrameChange, const QString&)>;

    mutable std::mutex m_mutex;
    bool m_ready = false;
    bool m_projectionEnabled = false;   // 动态投影总开关，默认关闭
    Frame m_frame;
    Display m_display;
    std::map<std::uint64_t, GeoFrameCallback> m_listeners;
    std::uint64_t m_listenerToken = 0;
    mutable QString m_lastError;
    CrsBackend m_backend;
};

} // namespace View3DCrs
