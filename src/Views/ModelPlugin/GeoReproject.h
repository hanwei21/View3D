#pragma once

#include <ModelMesh.h>

#include <QString>
#include <vector>

namespace View3D {
class IGeoReference;
}

namespace View3DModel {

// 导入时的参照系归一：源数据（EPSG:3857、UTM、CGCS2000、地方坐标系…）先批量转到
// 主参照系 (经度, 纬度, 椭球高)，再按显示参照系写成场景局部米。地理系显示时局部轴
// 是世界原点处的 ENU 切平面（真北朝上），投影系显示时局部轴是该投影的网格轴
// （网格北朝上）——两条都由 IGeoReference::lonLatHeightToLocal 负责，读取器不分支。
// 读取器现有的 toDisplay(x,y,z) 会把水平/高程写成 (E,U,-N)，于是矢量、影像底图与
// 3D Tiles 共用同一个原点，多源叠加不会互相错位。
// 参照系服务缺失或源没有 CRS 时一律返回 false 且不改动坐标，调用方保留源坐标并告警。

// 源参照系 -> 主参照系：xyz 按 (x,y,z) 连续三元组变换，成功后原地变成 (经度,纬度,高程)。
bool reprojectSourceToGeographic(View3D::IGeoReference* geoReference, const QString& sourceCrs,
                                 std::vector<double>* xyz, QString* error);

// 世界原点：先加载的地理数据源已钉住时直接复用（否则已提交的网格会整体漂移），
// 未钉住时用给定中心点钉住，高程取 0，真实高程留在局部 up 分量里。
bool ensureWorldOrigin(View3D::IGeoReference* geoReference, double centerLonDeg,
                       double centerLatDeg, QString* error);

// (经度,纬度,高程) -> 局部水平分量 (东,北/网格东,网格北,上)：outEnu 与输入等长，
// 任一点失败即整体失败、不写结果。
bool geographicToLocalEnu(View3D::IGeoReference* geoReference,
                          const std::vector<double>& geographic, std::vector<double>* outEnu,
                          QString* error);

// 参照系标识：优先 WKT 里的 EPSG 代码，退化到坐标系名称，用于日志与图层属性展示。
QString crsLabelOf(const QString& crs);

// ── 动态投影：源地理坐标缓存与重放 ─────────────────────────────────────────
// 导入归一后把局部米逐点反算成 (经度,纬度,椭球高) 存进 DTO；切换显示参照系时用这份
// 缓存按新映射重放，不必回头重读源文件。反算是逐点做的，顶点数超过上限就不缓存
// （由调用方改走「重新读取源文件」那条路），避免把内存吃穿。
constexpr std::size_t modelGeoCacheMaxVertices = 4'000'000;

// 写入 mesh 的 geoCrs / geoVertices / geoDisplayCrs；已有缓存时直接返回成功。
// 带节点变换的网格（顶点不在世界位置）不缓存，返回 false 并说明原因。
bool cacheGeographicVertices(const View3D::IGeoReference* geoReference,
                             View3D::ModelMeshDto* mesh, QString* error);

// 按当前显示参照系重放缓存的源地理坐标：改写顶点位置、让法线跟随投影旋转，
// 并重建射线索引。改动就地生效，调用方负责把新网格交给渲染端。
bool replayGeographicVertices(const View3D::IGeoReference* geoReference,
                              View3D::ModelMeshDto* mesh, QString* error);

} // namespace View3DModel
