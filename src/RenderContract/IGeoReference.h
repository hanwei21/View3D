#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <QString>
#include <vector>

namespace View3D {

// 地理参考（参照系）服务：主参照系是 WGS84 经纬度+椭球高（EPSG:4979 语义），
// 数据源各自的参照系（EPSG:3857、UTM、CGCS2000、地方坐标系…）先经 transformToMain
// 归到主参照系，再按「显示参照系」进场景局部坐标。
// 显示参照系决定局部轴的取向：地理系沿用世界原点处的 ENU 切平面（真北朝上），
// 投影系取该投影的网格轴（网格北朝上、单位米）；两者共用同一个世界原点，
// 影像底图、坐标导航、导入器与 3D Tiles 因此不会互相错位。实现见 plugin.view3d.crs。
class IGeoReference {
  public:
    virtual ~IGeoReference() = default;

    // 是否已初始化局部坐标系（加载了第一个地理数据源后变 true）。
    virtual bool ready() const noexcept = 0;

    // 设定世界原点：由第一个加载的地理数据源钉住，之后不随新数据移动，
    // 否则已经提交的网格会整体漂移。已钉住时返回 false 且不改动坐标系。
    virtual bool setOrigin(double lonDeg, double latDeg, double heightMeters) = 0;

    // 清空世界原点：卸载全部地理数据后调用，下一次加载重新钉住。
    virtual void resetOrigin() = 0;

    // 用 WGS84 ECEF 点钉住原点（3D Tiles 的包围体中心就是这种坐标）：
    // 与 setOrigin 同样的首钉语义，已钉住时返回 false。
    virtual bool setOriginFromEcef(const std::array<double, 3>& ecefMeters) = 0;

    // 经度(deg)、纬度(deg)、高程(m) -> 场景局部 XYZ(米, Y-up)。
    virtual bool lonLatHeightToLocal(double lonDeg, double latDeg, double heightMeters,
                                     std::array<double, 3>* localXyz) const = 0;

    // 场景局部 XYZ -> 经度(deg)、纬度(deg)、高程(m)。
    virtual bool localToLonLatHeight(const std::array<double, 3>& localXyz,
                                     double* lonDeg, double* latDeg,
                                     double* heightMeters) const = 0;

    // 水平参考框架标识，例如 "EPSG:4326" 或 "EPSG:3857"。
    virtual QString horizontalCrs() const = 0;

    // 主参照系标识：源数据一律先归到它，再进场景局部坐标。
    virtual QString mainCrs() const = 0;

    // 当前世界原点（未钉住时返回 false）。三个来源共用同一原点，
    // 后加载的数据不允许搬动已提交网格的位置。
    virtual bool origin(double* lonDeg, double* latDeg, double* heightMeters) const = 0;
    // WGS84 ECEF 原点（米）：3D Tiles 用它构造同一套 ENU 框架。
    virtual bool originEcef(std::array<double, 3>* ecefMeters) const = 0;

    // ── 球面模式支持：场景局部坐标 ↔ ECEF 的点换算 ────────────────────────
    // 走 localToLonLatHeight/lonLatHeightToLocal 同一条映射（显示参照系生效时
    // 也正确），供球面模式切换时的相机换算与渲染空间命中点反算。方向向量用
    // f(v) − f(0) 导出线性部分。原点未钉住或换算失败返回 false；默认实现返回
    // false，调用方据此降级。
    virtual bool localToEcefPoint(const std::array<double, 3>& localXyz,
                                  std::array<double, 3>* ecefMeters) const
    {
        static_cast<void>(localXyz);
        static_cast<void>(ecefMeters);
        return false;
    }
    virtual bool ecefToLocalPoint(const std::array<double, 3>& ecefMeters,
                                  std::array<double, 3>* localXyz) const
    {
        static_cast<void>(ecefMeters);
        static_cast<void>(localXyz);
        return false;
    }

    struct CrsInfo {
        QString name;
        QString code;
        bool geographic = false;  // 经纬度坐标系（度）
        bool cartesian = false;   // 投影/地心直角坐标（米）
        bool projected = false;   // 直角坐标里确实有平面轴的那一类
    };
    // 参照系探针：用于 UI 校验与展示，源可以是 "EPSG:3857" 或 OGC WKT。
    virtual bool crsInfo(const QString& sourceCrs, CrsInfo* info) const = 0;

    // 源参照系 → 主参照系的原地批量变换：xyz 按 (x,y,z) 三元组连续存放，
    // 变换后为 (经度 deg, 纬度 deg, 椭球高 m)；投影系的 z 当作高程，缺省传 0。
    // 参照系无效或后端不可用时返回 false 且不改动数组，调用方需保留原坐标并告警。
    virtual bool transformToMain(const QString& sourceCrs, double* xyz,
                                 std::size_t pointCount) const = 0;

    // ── 动态投影：运行期可切换的显示参照系 ────────────────────────────────
    // 局部坐标轴的取向由显示参照系决定：地理系（EPSG:4326/4979）沿用世界原点处的
    // ENU 切平面（真北朝上），投影系（EPSG:3857、UTM、CGCS2000 高斯-克吕格…）把
    // 局部轴取为该投影的网格轴（网格北朝上、单位是网格米）——lonLatHeightToLocal /
    // localToLonLatHeight 与逐图层重放都走同一套映射，因此不会互相错位。
    //
    // 整套行为由「动态投影开关」控制，默认关闭：关闭时局部系就是世界原点处的 ENU
    // 切平面，流式成果（倾斜摄影、3D Tiles）保持各自成果坐标系下的摆放，不做收敛角
    // 旋转与比例改正，导入也不再缓存源坐标——与该特性实现之前的行为逐字一致。
    // 打开后局部轴跟随所选显示参照系，瓦片按原点处的收敛角+比例因子刚体跟随，
    // 带源坐标缓存的图层按新参照系原地重放。显式 setDisplayCrs 视为打开开关。
    virtual bool dynamicProjectionEnabled() const = 0;
    // 关到开、开到关都会发 GeoFrameChange 通知（重放与瓦片跟随据此复位或重算）。
    virtual bool setDynamicProjectionEnabled(bool enabled) = 0;

    // 当前生效的显示参照系；开关关闭或没切换过时返回主参照系，即 ENU 显示（真北朝上）。
    virtual QString displayCrs() const = 0;

    // 上一次切换显示参照系失败的原因，供 UI 直接引用；默认空。
    virtual QString lastError() const { return {}; }

    // 切换显示参照系：接受 EPSG/ESRI 代码或 OGC WKT。只接受地理系与投影系；
    // 地心直角（EPSG:4978）这类没有平面上轴的参照系返回 false 且不改动当前值。
    // 世界原点（经纬度）不随切换移动，改的只是局部轴的取向。
    virtual bool setDisplayCrs(const QString& crs) = 0;

    struct PlaneFrame {
        // 真北切平面 (东,北) 米 -> 该参照系网格 (东,北) 米的 2×2，行主序 [a00,a01,a10,a11]。
        // 地理系/无投影后端时是单位阵。
        std::array<double, 4> trueToGrid { {1.0, 0.0, 0.0, 1.0} };
        double scaleFactor = 1.0;      // 投影比例因子 k：网格长度 / 地面长度
        double convergenceDeg = 0.0;   // 网格北相对真北的收敛角，东偏为正
    };
    // 给定参照系在某个地理位置处的平面微分：法线旋转、以及瓦片类数据「刚体跟随
    // 投影」都靠它，投影的非线性在这个小邻域内用该线性近似代替。
    virtual bool planeFrameAt(const QString& crs, double lonDeg, double latDeg,
                              double heightMeters, PlaneFrame* frame) const = 0;

    struct PlanePlacement {
        double yawDegrees = 0.0;        // 绕 up 轴（+Y，右手）的旋转
        double horizontalScale = 1.0;   // 水平各向同性缩放
        // 源局部原点在当前显示参照系下的场景局部坐标；未钉世界原点时为 (0,0,0)。
        std::array<double, 3> originLocal { {0.0, 0.0, 0.0} };
    };
    // 把「按 sourceCrs 的平面轴摆放、局部原点在该经纬度处」的流式数据（倾斜摄影、
    // 3D Tiles）刚体跟到当前显示参照系下：源网格轴 → 显示网格轴的相似变换 + 平移。
    // 源局部系就是真北切平面（3D Tiles 的 ENU）时，sourceCrs 传地理系标识即可。
    // 源参照系认不出来时返回 false，调用方应保持原摆放。
    virtual bool planePlacement(const QString& sourceCrs, double sourceOriginLonDeg,
                                double sourceOriginLatDeg, double sourceOriginHeightMeters,
                                PlanePlacement* placement) const = 0;

    // 源参照系三元组 -> 当前显示参照系下的场景局部 XYZ(米, Y-up) 批量换算，
    // 与逐点 lonLatHeightToLocal 等价，只是少一次锁与管线查找。
    // sourceCrs 为空表示输入已是主参照系（经纬度+椭球高）。失败不写 out。
    virtual bool sourcePointsToLocal(const QString& sourceCrs, const double* xyz,
                                     std::size_t pointCount,
                                     std::vector<double>* localXyz) const = 0;

    // 参照系框架变化通知：显示参照系切换、世界原点首次钉住或重置都会触发。
    // 逐点重放的图层只关心 DisplayCrs（原点在导入时已经用上了）；只按整体矩阵摆放
    // 的流式数据（倾斜摄影）两种都要处理，否则原点换了它会跟别的图层错位。
    // token 用于移除；回调在触发线程上同步执行，耗时操作应自己投回工作线程。
    enum class GeoFrameChange { DisplayCrs, WorldOrigin };
    virtual std::uint64_t addGeoFrameChangeListener(
        std::function<void(GeoFrameChange change, const QString& displayCrs)> callback) = 0;
    virtual void removeGeoFrameChangeListener(std::uint64_t token) = 0;
};

} // namespace View3D
