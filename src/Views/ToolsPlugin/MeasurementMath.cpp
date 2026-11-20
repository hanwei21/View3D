#include "MeasurementMath.h"

#include <GlobeMath.h>

#include <QtMath>

#include <cmath>

namespace View3DPlugins {
namespace {

bool finite(const MeasurePoint& point)
{
    for (double value : point.position) {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

// 球面读数：以给定 ECEF 点为原点建 ENU 基（东、北、天），把附近点投到局部平面。
// 城市尺度的测量用这一小段切平面近似足够（10 km 量级的面积误差在厘米级）。
bool ecefEnuFrame(const std::array<double, 3>& origin, std::array<double, 3>* east,
                  std::array<double, 3>* north, std::array<double, 3>* up)
{
    using namespace View3D::GlobeMath;
    *up = ecefNormal(origin);
    if (length(*up) < 1e-9)
        return false;
    *east = normalized(cross({0.0, 0.0, 1.0}, *up));
    if (length(*east) < 1e-6)
        *east = {1.0, 0.0, 0.0};
    *north = normalized(cross(*up, *east));
    return length(*east) > 0.5 && length(*north) > 0.5;
}

// ECEF 点 → 以 origin 为原点的局部 (东, 北, 天) 坐标。
bool ecefToEnu(const std::array<double, 3>& point, const std::array<double, 3>& origin,
               std::array<double, 3>* enu)
{
    using namespace View3D::GlobeMath;
    std::array<double, 3> east {};
    std::array<double, 3> north {};
    std::array<double, 3> up {};
    if (!ecefEnuFrame(origin, &east, &north, &up))
        return false;
    const std::array<double, 3> delta = sub(point, origin);
    *enu = { dot(delta, east), dot(delta, north), dot(delta, up) };
    return true;
}

double distance3d(const std::array<double, 3>& first, const std::array<double, 3>& second)
{
    const double dx = second[0] - first[0];
    const double dy = second[1] - first[1];
    const double dz = second[2] - first[2];
    return std::hypot(std::hypot(dx, dy), dz);
}

// 水平面距离：Y 是高程，水平面由 X（东）与 Z（北，取 -Z 朝向北）张成。
double planarDistance(const std::array<double, 3>& first, const std::array<double, 3>& second)
{
    return std::hypot(second[0] - first[0], second[2] - first[2]);
}

double segmentLength(const std::array<double, 3>& first, const std::array<double, 3>& second,
                     MeasureAxes axes)
{
    return axes == MeasureAxes::Planar ? planarDistance(first, second)
                                       : distance3d(first, second);
}

// 平面方向积固定取水平投影 (X, Z)；符号在这里只用于左右关系，取 -Z 还是 +Z 不影响
// 面积与自相交判定的正确性。
double orientation2d(const std::array<double, 3>& a, const std::array<double, 3>& b,
                     const std::array<double, 3>& c)
{
    return (b[0] - a[0]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[0] - a[0]);
}

bool properIntersection(const std::array<double, 3>& a, const std::array<double, 3>& b,
                        const std::array<double, 3>& c, const std::array<double, 3>& d)
{
    const double abc = orientation2d(a, b, c);
    const double abd = orientation2d(a, b, d);
    const double cda = orientation2d(c, d, a);
    const double cdb = orientation2d(c, d, b);
    return ((abc > 1e-12 && abd < -1e-12) || (abc < -1e-12 && abd > 1e-12)) &&
        ((cda > 1e-12 && cdb < -1e-12) || (cda < -1e-12 && cdb > 1e-12));
}

bool selfIntersects(const QList<MeasurePoint>& points)
{
    const int count = points.size();
    for (int i = 0; i < count; ++i) {
        const int iNext = (i + 1) % count;
        for (int j = i + 1; j < count; ++j) {
            const int jNext = (j + 1) % count;
            if (i == j || iNext == j || jNext == i)
                continue;
            if (properIntersection(points[i].position, points[iNext].position,
                                   points[j].position, points[jNext].position))
                return true;
        }
    }
    return false;
}

} // namespace

MeasurePoint snapToGeometry(
    const MeasurePoint& hitPoint,
    const std::array<std::array<double, 3>, 3>& triangleVertices, double tolerance,
    bool enabled, MeasureAxes axes)
{
    if (!enabled || !finite(hitPoint) || !qIsFinite(tolerance) || tolerance < 0.0)
        return hitPoint;
    MeasurePoint best = hitPoint;
    double nearest = tolerance;
    const auto consider = [&](const std::array<double, 3>& candidate) {
        const double distance = segmentLength(hitPoint.position, candidate, axes);
        if (distance <= nearest) {
            nearest = distance;
            best.position = candidate;
        }
    };
    for (const auto& vertex : triangleVertices)
        consider(vertex);
    for (int index = 0; index < 3; ++index) {
        const auto& first = triangleVertices[index];
        const auto& second = triangleVertices[(index + 1) % 3];
        consider({(first[0] + second[0]) * 0.5, (first[1] + second[1]) * 0.5,
                  (first[2] + second[2]) * 0.5});
    }
    return best;
}

QString formatMeasurePoint(const MeasurePoint& point, MeasureAxes axes)
{
    if (axes == MeasureAxes::Globe) {
        double lonDeg = 0.0;
        double latDeg = 0.0;
        double heightMeters = 0.0;
        if (View3D::GlobeMath::ecefToGeodetic(point.position, &lonDeg, &latDeg,
                                              &heightMeters)) {
            return QStringLiteral("(%1°, %2°, %3 m)")
                .arg(lonDeg, 0, 'f', 6)
                .arg(latDeg, 0, 'f', 6)
                .arg(heightMeters, 0, 'f', 2);
        }
        return QStringLiteral("(-)");
    }
    if (axes == MeasureAxes::Planar) {
        // 二维读数只有平面两轴：纵轴取 -Z，即数据里的北方向 Y。
        return QStringLiteral("(%1, %2)")
            .arg(point.position[0], 0, 'f', 3)
            .arg(-point.position[2], 0, 'f', 3);
    }
    return QStringLiteral("(%1, %2, %3)")
        .arg(point.position[0], 0, 'f', 3)
        .arg(point.position[1], 0, 'f', 3)
        .arg(point.position[2], 0, 'f', 3);
}

MeasureResult coordinateResult(const MeasurePoint& point, MeasureAxes axes)
{
    MeasureResult result;
    result.kind = MeasureKind::Coordinate;
    result.typeName = axes == MeasureAxes::Planar
        ? QStringLiteral("平面坐标")
        : axes == MeasureAxes::Globe ? QStringLiteral("经纬度") : QStringLiteral("坐标");
    result.axes = axes;
    result.points = {point};
    result.valid = finite(point);
    if (!result.valid)
        return result;
    if (axes == MeasureAxes::Globe) {
        double lonDeg = 0.0;
        double latDeg = 0.0;
        double heightMeters = 0.0;
        result.valid = View3D::GlobeMath::ecefToGeodetic(point.position, &lonDeg, &latDeg,
                                                         &heightMeters);
        if (result.valid) {
            result.summary = QStringLiteral("经度 %1°, 纬度 %2°, 椭球高 %3 m")
                                 .arg(lonDeg, 0, 'f', 8)
                                 .arg(latDeg, 0, 'f', 8)
                                 .arg(heightMeters, 0, 'f', 3);
        }
        return result;
    }
    result.summary = axes == MeasureAxes::Planar
        ? QStringLiteral("X=%1, Y=%2（平面坐标）")
              .arg(point.position[0], 0, 'f', 3)
              .arg(-point.position[2], 0, 'f', 3)
        : QStringLiteral("X=%1, Y=%2, Z=%3（场景坐标）")
              .arg(point.position[0], 0, 'f', 3)
              .arg(point.position[1], 0, 'f', 3)
              .arg(point.position[2], 0, 'f', 3);
    return result;
}

MeasureResult distanceResult(const MeasurePoint& first, const MeasurePoint& second,
                             MeasureAxes axes)
{
    MeasureResult result;
    result.kind = MeasureKind::Distance;
    result.typeName = axes == MeasureAxes::Planar ? QStringLiteral("平面距离")
                                                  : QStringLiteral("距离");
    result.axes = axes;
    result.points = {first, second};
    result.valid = finite(first) && finite(second);
    if (result.valid) {
        const double value = segmentLength(first.position, second.position, axes);
        result.valid = qIsFinite(value);
        result.summary = QStringLiteral("距离 %1 m").arg(value, 0, 'f', 3);
    }
    return result;
}

MeasureResult polylineResult(const QList<MeasurePoint>& points, MeasureAxes axes)
{
    MeasureResult result;
    result.kind = MeasureKind::Polyline;
    result.typeName = axes == MeasureAxes::Planar ? QStringLiteral("平面折线")
                                                  : QStringLiteral("折线");
    result.axes = axes;
    result.points = points;
    if (points.size() < 2)
        return result;
    for (const MeasurePoint& point : points) {
        if (!finite(point))
            return result;
    }
    if (axes == MeasureAxes::Globe) {
        // 球面：总长取 ECEF 弦长，水平/垂直按每段起点的 ENU 分解，高程差用椭球高。
        double total = 0.0;
        double horizontal = 0.0;
        double vertical = 0.0;
        for (int index = 1; index < points.size(); ++index) {
            const auto& first = points[index - 1].position;
            const auto& second = points[index].position;
            total += distance3d(first, second);
            std::array<double, 3> enu {};
            if (ecefToEnu(second, first, &enu)) {
                horizontal += std::hypot(enu[0], enu[1]);
                vertical += std::abs(enu[2]);
            }
        }
        double lon = 0.0;
        double lat = 0.0;
        double firstHeight = 0.0;
        double lastLon = 0.0;
        double lastLat = 0.0;
        double lastHeight = 0.0;
        const bool heights = View3D::GlobeMath::ecefToGeodetic(
                                 points.first().position, &lon, &lat, &firstHeight) &&
            View3D::GlobeMath::ecefToGeodetic(points.last().position, &lastLon, &lastLat,
                                              &lastHeight);
        result.valid = qIsFinite(total) && qIsFinite(horizontal) && qIsFinite(vertical) &&
            heights;
        if (result.valid) {
            result.summary =
                QStringLiteral("折线 %1 m；水平 %2；垂直累计 %3；高程差 %4")
                    .arg(total, 0, 'f', 3)
                    .arg(horizontal, 0, 'f', 3)
                    .arg(vertical, 0, 'f', 3)
                    .arg(lastHeight - firstHeight, 0, 'f', 3);
        }
        return result;
    }
    double total = 0.0;
    double horizontal = 0.0;
    double vertical = 0.0;
    for (int index = 1; index < points.size(); ++index) {
        const auto& first = points[index - 1].position;
        const auto& second = points[index].position;
        total += segmentLength(first, second, axes);
        if (axes == MeasureAxes::Spatial3D) {
            horizontal += planarDistance(first, second);
            vertical += std::abs(second[1] - first[1]);
        }
    }
    if (axes == MeasureAxes::Planar) {
        result.valid = qIsFinite(total);
        if (result.valid)
            result.summary = QStringLiteral("折线 %1 m").arg(total, 0, 'f', 3);
        return result;
    }
    // 高程是 Y 轴，不是 Z：Z 是北方向，之前把它当高程算，四个数全是错的。
    const double elevation = points.last().position[1] - points.first().position[1];
    result.valid = qIsFinite(total) && qIsFinite(horizontal) && qIsFinite(vertical) &&
        qIsFinite(elevation);
    if (result.valid) {
        result.summary =
            QStringLiteral("折线 %1 m；水平 %2；垂直累计 %3；高程差 %4")
                .arg(total, 0, 'f', 3)
                .arg(horizontal, 0, 'f', 3)
                .arg(vertical, 0, 'f', 3)
                .arg(elevation, 0, 'f', 3);
    }
    return result;
}

MeasureResult angleResult(const MeasurePoint& first, const MeasurePoint& vertex,
                          const MeasurePoint& third, MeasureAxes axes)
{
    MeasureResult result;
    result.kind = MeasureKind::Angle;
    result.typeName = axes == MeasureAxes::Planar ? QStringLiteral("平面角度")
                                                  : QStringLiteral("角度");
    result.axes = axes;
    result.points = {first, vertex, third};
    if (!finite(first) || !finite(vertex) || !finite(third))
        return result;
    // 平面模式只取水平两轴，斜着的高程差不再影响夹角。
    int usedAxes[3] = {0, 2, 0};
    int usedCount = 3;
    if (axes == MeasureAxes::Planar) {
        usedAxes[0] = 0;
        usedAxes[1] = 2;
        usedCount = 2;
    } else {
        usedAxes[0] = 0;
        usedAxes[1] = 1;
        usedAxes[2] = 2;
    }
    double firstLengthSquared = 0.0;
    double secondLengthSquared = 0.0;
    double dot = 0.0;
    for (int index = 0; index < usedCount; ++index) {
        const int axis = usedAxes[index];
        const double firstDelta = first.position[axis] - vertex.position[axis];
        const double secondDelta = third.position[axis] - vertex.position[axis];
        firstLengthSquared += firstDelta * firstDelta;
        secondLengthSquared += secondDelta * secondDelta;
        dot += firstDelta * secondDelta;
    }
    if (firstLengthSquared <= 1e-18 || secondLengthSquared <= 1e-18)
        return result;
    const double cosine =
        qBound(-1.0, dot / std::sqrt(firstLengthSquared * secondLengthSquared), 1.0);
    const double value = qRadiansToDegrees(std::acos(cosine));
    result.valid = qIsFinite(value);
    if (result.valid)
        result.summary = QStringLiteral("角度 %1°（顶点为第 2 点）").arg(value, 0, 'f', 3);
    return result;
}

MeasureResult areaResult(const QList<MeasurePoint>& points, MeasureAxes axes)
{
    MeasureResult result;
    result.kind = MeasureKind::Area;
    result.typeName = QStringLiteral("面积");
    result.axes = axes;
    result.points = points;
    result.closedPolygon = true;
    if (points.size() < 3)
        return result;
    for (const MeasurePoint& point : points) {
        if (!finite(point))
            return result;
    }
    if (axes == MeasureAxes::Globe) {
        // 球面：以多边形质心处的 ENU 切平面投影，再用鞋带公式算面积；周长取弦长。
        std::array<double, 3> center { 0.0, 0.0, 0.0 };
        for (const MeasurePoint& point : points) {
            for (int axis = 0; axis < 3; ++axis)
                center[axis] += point.position[axis];
        }
        for (double& value : center)
            value /= static_cast<double>(points.size());
        QList<MeasurePoint> projected;
        projected.reserve(points.size());
        for (const MeasurePoint& point : points) {
            std::array<double, 3> enu {};
            if (!ecefToEnu(point.position, center, &enu)) {
                result.summary = QStringLiteral("无效多边形：椭球局部投影失败");
                return result;
            }
            projected.append(MeasurePoint { { enu[0], 0.0, -enu[1] } });
        }
        if (selfIntersects(projected)) {
            result.summary = QStringLiteral("无效多边形：水平投影边界存在自相交");
            return result;
        }
        double twiceArea = 0.0;
        double perimeter = 0.0;
        for (int index = 0; index < projected.size(); ++index) {
            const auto& first = projected[index].position;
            const auto& second = projected[(index + 1) % projected.size()].position;
            twiceArea += first[0] * second[2] - second[0] * first[2];
            perimeter += distance3d(points[index].position,
                                    points[(index + 1) % points.size()].position);
        }
        const double area = std::abs(twiceArea) * 0.5;
        result.valid = area > 1e-12 && qIsFinite(area) && qIsFinite(perimeter);
        if (result.valid) {
            result.summary = QStringLiteral("面积 %1 m²；周长 %2 m（椭球局部平面）")
                                 .arg(area, 0, 'f', 3)
                                 .arg(perimeter, 0, 'f', 3);
        }
        return result;
    }
    if (selfIntersects(points)) {
        result.summary = QStringLiteral("无效多边形：水平投影边界存在自相交");
        return result;
    }
    double twiceArea = 0.0;
    double perimeter = 0.0;
    for (int index = 0; index < points.size(); ++index) {
        const auto& first = points[index].position;
        const auto& second = points[(index + 1) % points.size()].position;
        // 鞋带公式取 (X, Z)：原来的 (X, Y) 量的是"东×高程"的投影面积。
        twiceArea += first[0] * second[2] - second[0] * first[2];
        perimeter += planarDistance(first, second);
    }
    const double area = std::abs(twiceArea) * 0.5;
    result.valid = area > 1e-12 && qIsFinite(area) && qIsFinite(perimeter);
    if (result.valid) {
        result.summary = axes == MeasureAxes::Planar
            ? QStringLiteral("面积 %1 m²；周长 %2 m")
                  .arg(area, 0, 'f', 3)
                  .arg(perimeter, 0, 'f', 3)
            : QStringLiteral("面积 %1 m²；周长 %2 m（水平投影）")
                  .arg(area, 0, 'f', 3)
                  .arg(perimeter, 0, 'f', 3);
    }
    return result;
}

QString measureToolCaption(MeasureTool tool)
{
    switch (tool) {
    case MeasureTool::Coordinate:
        return QStringLiteral("坐标拾取");
    case MeasureTool::Distance:
        return QStringLiteral("直线距离");
    case MeasureTool::Polyline:
        return QStringLiteral("折线距离");
    case MeasureTool::Angle:
        return QStringLiteral("三点角度");
    case MeasureTool::Area:
        return QStringLiteral("面积与周长");
    default:
        return {};
    }
}

QString measureUsageHint(MeasureTool tool)
{
    switch (tool) {
    case MeasureTool::Coordinate:
        return QStringLiteral("左键点击模型表面读取坐标，Esc 或视口右键退出。");
    case MeasureTool::Distance:
        return QStringLiteral("依次左键拾取两个点，第二点落下即出结果；Esc 或视口右键退出。");
    case MeasureTool::Polyline:
        return QStringLiteral("连续左键拾取，视口右键选「完成折线」保存；Esc 退出不保存。");
    case MeasureTool::Angle:
        return QStringLiteral("依次左键拾取起点、顶点和终点，第三点落下即出结果；Esc 或视口右键退出。");
    case MeasureTool::Area:
        return QStringLiteral("连续左键拾取顶点，视口右键选「完成多边形」闭合；Esc 退出不保存。");
    default:
        return QStringLiteral("选择一个测量工具后，左键拾取模型表面。");
    }
}

} // namespace View3DPlugins
