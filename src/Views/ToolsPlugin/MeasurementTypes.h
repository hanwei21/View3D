#pragma once

#include <QList>
#include <QString>

#include <array>

namespace View3DPlugins {

enum class MeasureTool {
    None = -1,
    Coordinate = 0,
    Distance,
    Polyline,
    Angle,
    Area
};

struct MeasurePoint {
    std::array<double, 3> position {};
};

// 场景显示约定是 Y-up：X=东、Y=高程、-Z=北。测量按模式取不同的轴对，
// 二维模式只用水平面 (X, -Z)，结果里不再出现高程。球面模式下点存 ECEF，
// 读数按经纬度/椭球高与弦长换算。
enum class MeasureAxes { Spatial3D = 0, Planar = 1, Globe = 2 };

enum class MeasureKind { Coordinate, Distance, Polyline, Angle, Area };

struct MeasureResult {
    MeasureKind kind = MeasureKind::Coordinate;
    QString typeName;
    QString summary;
    QList<MeasurePoint> points;
    MeasureAxes axes = MeasureAxes::Spatial3D;
    bool closedPolygon = false;
    bool valid = false;
};

// 供 Ribbon 命令读取/切换当前测量工具。命令与工具体都在本插件 DLL 内。
class IMeasureToolService {
  public:
    virtual ~IMeasureToolService() = default;

    virtual void setMeasureTool(MeasureTool tool, bool active) = 0;
    virtual void setSnappingEnabled(bool enabled) = 0;
    virtual bool measureToolActive(MeasureTool tool) const = 0;
    virtual bool snappingEnabled() const = 0;
};

inline const QString MeasureToolServiceName = QStringLiteral("view3d.tools.measure");

} // namespace View3DPlugins
