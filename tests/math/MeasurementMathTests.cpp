#include "MeasurementMath.h"

#include <GlobeMath.h>

#include <QString>

#include <cmath>
#include <cstdio>
#include <vector>

// 测量数学的纯函数回归：没有渲染、没有视口，只验证轴口径。
// 场景显示约定是 Y-up：X=东、Y=高程、-Z=北，所以水平面是 (X, Z)。
namespace {

int checks = 0;
int failures = 0;
std::vector<QString> failedMessages;

void checkTrue(const char* expression, const bool value, const QString& detail = {})
{
    ++checks;
    if (value)
        return;
    ++failures;
    QString message = QStringLiteral("断言失败：%1").arg(QString::fromUtf8(expression));
    if (!detail.isEmpty())
        message += QStringLiteral("（%1）").arg(detail);
    failedMessages.push_back(message);
}

void checkNear(const char* expression, const double actual, const double expected,
               const double tolerance = 1e-6)
{
    const bool ok = std::abs(actual - expected) <= tolerance;
    checkTrue(expression, ok,
              QStringLiteral("期望 %1，实际 %2").arg(expected, 0, 'f', 6).arg(actual, 0, 'f', 6));
}

double numberIn(const QString& text, const QString& label)
{
    const int start = text.indexOf(label);
    if (start < 0)
        return -1.0;
    // 标签后面紧跟数字，可能带单位或百分号尾巴：只取开头的数字片段。
    QString digits;
    for (const QChar character : text.mid(start + label.size())) {
        if (digits.isEmpty() && character == QLatin1Char('-'))
            digits.append(character);
        else if (character.isDigit() || character == QLatin1Char('.'))
            digits.append(character);
        else
            break;
    }
    bool ok = false;
    const double value = digits.toDouble(&ok);
    return ok ? value : -1.0;
}

// 竖直面里的两点：Y 差 4 是纯高程差。
View3DPlugins::MeasurePoint point(const double x, const double y, const double z)
{
    View3DPlugins::MeasurePoint result;
    result.position = {x, y, z};
    return result;
}

using View3DPlugins::areaResult;
using View3DPlugins::angleResult;
using View3DPlugins::coordinateResult;
using View3DPlugins::distanceResult;
using View3DPlugins::formatMeasurePoint;
using View3DPlugins::MeasureAxes;
using View3DPlugins::MeasurePoint;
using View3DPlugins::polylineResult;
using View3DPlugins::snapToGeometry;

void testDistance()
{
    // 三维距离含高程：(0,0,0)→(3,4,0) 是 5。
    const auto spatial = distanceResult(point(0, 0, 0), point(3, 4, 0), MeasureAxes::Spatial3D);
    checkTrue("distance spatial valid", spatial.valid);
    checkNear("distance spatial 5", numberIn(spatial.summary, QStringLiteral("距离 ")), 5.0, 1e-6);

    // 三维距离按 X-Z 也一样是 5：证明三个轴都进了公式。
    const auto spatialZ = distanceResult(point(0, 0, 0), point(3, 0, 4), MeasureAxes::Spatial3D);
    checkNear("distance spatial xz 5", numberIn(spatialZ.summary, QStringLiteral("距离 ")), 5.0,
              1e-6);

    // 平面距离只看 X-Z：纯高程差 (0,5,0)→(0,0,0) 应该是 0。
    const auto planar = distanceResult(point(0, 5, 0), point(0, 0, 0), MeasureAxes::Planar);
    checkTrue("distance planar valid", planar.valid);
    checkNear("distance planar ignores height",
              numberIn(planar.summary, QStringLiteral("距离 ")), 0.0, 1e-6);

    const auto planarMixed = distanceResult(point(0, 9, 0), point(3, 4, 4), MeasureAxes::Planar);
    checkNear("distance planar 5", numberIn(planarMixed.summary, QStringLiteral("距离 ")), 5.0,
              1e-6);
}

void testCoordinateText()
{
    const MeasurePoint sample = point(2, 9, -3);
    const auto spatial = coordinateResult(sample, MeasureAxes::Spatial3D);
    checkTrue("coordinate spatial shows three axes",
              spatial.summary.contains(QStringLiteral("Y=9.000")) &&
                  spatial.summary.contains(QStringLiteral("Z=-3.000")),
              spatial.summary);

    // 二维读数只有两轴，纵轴取 -Z（北）：(2, 9, -3) → X=2, Y=3。
    const auto planar = coordinateResult(sample, MeasureAxes::Planar);
    checkTrue("coordinate planar hides height axis",
              planar.valid && planar.summary.contains(QStringLiteral("X=2.000")) &&
                  planar.summary.contains(QStringLiteral("Y=3.000")) &&
                  !planar.summary.contains(QStringLiteral("Z=")),
              planar.summary);

    checkTrue("format planar two components",
              formatMeasurePoint(sample, MeasureAxes::Planar) ==
                  QStringLiteral("(2.000, 3.000)"),
              formatMeasurePoint(sample, MeasureAxes::Planar));
    checkTrue("format spatial three components",
              formatMeasurePoint(sample, MeasureAxes::Spatial3D) ==
                  QStringLiteral("(2.000, 9.000, -3.000)"),
              formatMeasurePoint(sample, MeasureAxes::Spatial3D));
}

void testPolyline()
{
    const QList<MeasurePoint> points {point(0, 0, 0), point(0, 3, 4)};

    // 高程在 Y：这一段水平位移是 4（Z 方向），垂直位移是 3（Y 方向）。
    const auto spatial = polylineResult(points, MeasureAxes::Spatial3D);
    checkTrue("polyline spatial valid", spatial.valid);
    checkNear("polyline spatial total", numberIn(spatial.summary, QStringLiteral("折线 ")), 5.0,
              1e-6);
    checkNear("polyline spatial horizontal", numberIn(spatial.summary, QStringLiteral("水平 ")),
              4.0, 1e-6);
    checkNear("polyline spatial vertical",
              numberIn(spatial.summary, QStringLiteral("垂直累计 ")), 3.0, 1e-6);
    checkNear("polyline spatial elevation",
              numberIn(spatial.summary, QStringLiteral("高程差 ")), 3.0, 1e-6);

    // 二维只看水平面：总长就是 4，并且不再出现高程字段。
    const auto planar = polylineResult(points, MeasureAxes::Planar);
    checkTrue("polyline planar valid", planar.valid);
    checkNear("polyline planar total", numberIn(planar.summary, QStringLiteral("折线 ")), 4.0,
              1e-6);
    checkTrue("polyline planar drops elevation",
              !planar.summary.contains(QStringLiteral("高程差")) &&
                  !planar.summary.contains(QStringLiteral("垂直累计")),
              planar.summary);
}

void testArea()
{
    // 带高程抖动的水平正方形：边长 4，面积 16、周长 16，高程不参与。
    const QList<MeasurePoint> square {
        point(0, 0, 0), point(4, 7, 0), point(4, 2, 4), point(0, 5, 4)};
    for (const MeasureAxes axes : {MeasureAxes::Spatial3D, MeasureAxes::Planar}) {
        const auto area = areaResult(square, axes);
        checkTrue("area valid", area.valid);
        checkNear("area 16", numberIn(area.summary, QStringLiteral("面积 ")), 16.0, 1e-6);
        checkNear("perimeter 16", numberIn(area.summary, QStringLiteral("周长 ")), 16.0, 1e-6);
    }

    // 水平投影里的蝴蝶结：必须判成自相交，而不是靠高程躲开。
    const QList<MeasurePoint> bowtie {
        point(0, 0, 0), point(2, 4, 2), point(0, 8, 2), point(2, 1, 0)};
    const auto invalid = areaResult(bowtie, MeasureAxes::Planar);
    checkTrue("bowtie rejected", !invalid.valid &&
                                     invalid.summary.contains(QStringLiteral("自相交")),
                invalid.summary);
}

void testAngle()
{
    // a=(1,1,0)、c=(1,-1,0)：三维里互相垂直，投到水平面后重合为 0°。
    const auto spatial = angleResult(point(1, 1, 0), point(0, 0, 0), point(1, -1, 0),
                                     MeasureAxes::Spatial3D);
    checkNear("angle spatial 90", numberIn(spatial.summary, QStringLiteral("角度 ")), 90.0, 1e-6);
    const auto planar = angleResult(point(1, 1, 0), point(0, 0, 0), point(1, -1, 0),
                                    MeasureAxes::Planar);
    checkNear("angle planar 0", numberIn(planar.summary, QStringLiteral("角度 ")), 0.0, 1e-6);
}

void testSnapping()
{
    // 候选顶点 (0,5,0) 只在高程上差 5、平面位置与命中点重合：
    // 三维里它超出容差（直线距离 5），平面里它距离 0，是最近候选。
    const std::array<std::array<double, 3>, 3> triangle {
        {{0.0, 5.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}};
    const MeasurePoint hit = point(0, 0, 0);
    const auto spatial = snapToGeometry(hit, triangle, 3.0, true, MeasureAxes::Spatial3D);
    checkTrue("snap spatial skips far height vertex", spatial.position[1] != 5.0,
              formatMeasurePoint(spatial, MeasureAxes::Spatial3D));
    const auto planar = snapToGeometry(hit, triangle, 3.0, true, MeasureAxes::Planar);
    checkTrue("snap planar takes height vertex",
              planar.position == point(0, 5, 0).position,
              formatMeasurePoint(planar, MeasureAxes::Spatial3D));
}

// 球面读数：测量点存 ECEF，坐标给经纬度+椭球高，距离用弦长，面积投到局部 ENU。
View3DPlugins::MeasurePoint ecefPoint(double lonDeg, double latDeg, double heightMeters,
                                      double east, double north, double up)
{
    using namespace View3D::GlobeMath;
    const Vec3 base = geodeticToEcef(lonDeg, latDeg, heightMeters);
    const Vec3 upVector = ecefNormal(base);
    Vec3 eastVector = normalized(cross({ 0.0, 0.0, 1.0 }, upVector));
    if (length(eastVector) < 1e-6)
        eastVector = { 1.0, 0.0, 0.0 };
    const Vec3 northVector = normalized(cross(upVector, eastVector));
    const Vec3 position =
        add(base, add(scale(eastVector, east),
                      add(scale(northVector, north), scale(upVector, up))));
    View3DPlugins::MeasurePoint result;
    result.position = position;
    return result;
}

void testGlobe()
{
    const double lon = 116.39;
    const double lat = 39.91;
    const auto coordinate =
        coordinateResult(ecefPoint(lon, lat, 0.0, 0.0, 0.0, 0.0), MeasureAxes::Globe);
    checkTrue("globe coordinate valid", coordinate.valid);
    checkNear("globe coordinate lon", numberIn(coordinate.summary, QStringLiteral("经度 ")),
              lon, 1e-6);
    checkNear("globe coordinate lat", numberIn(coordinate.summary, QStringLiteral("纬度 ")),
              lat, 1e-6);
    checkNear("globe coordinate height",
              numberIn(coordinate.summary, QStringLiteral("椭球高 ")), 0.0, 1e-3);
    checkTrue("globe point text",
              formatMeasurePoint(coordinate.points.first(), MeasureAxes::Globe)
                  .contains(QStringLiteral("°")));

    // 正东 100 m：弦长就是 100 m。
    const auto distance = distanceResult(ecefPoint(lon, lat, 0, 0, 0, 0),
                                         ecefPoint(lon, lat, 0, 100, 0, 0),
                                         MeasureAxes::Globe);
    checkNear("globe distance 100",
              numberIn(distance.summary, QStringLiteral("距离 ")), 100.0, 1e-3);

    // 折线：水平 100 m、高差 0.5 m。
    const QList<MeasurePoint> line { ecefPoint(lon, lat, 0, 0, 0, 0),
                                     ecefPoint(lon, lat, 0, 100, 0, 0.5) };
    const auto polyline = polylineResult(line, MeasureAxes::Globe);
    checkTrue("globe polyline valid", polyline.valid);
    checkNear("globe polyline horizontal",
              numberIn(polyline.summary, QStringLiteral("水平 ")), 100.0, 1e-2);
    checkNear("globe polyline elevation",
              numberIn(polyline.summary, QStringLiteral("高程差 ")), 0.5, 1e-2);

    // 面积：ENU 切平面里 100×100 的正方形。
    const QList<MeasurePoint> square {
        ecefPoint(lon, lat, 0, 0, 0, 0), ecefPoint(lon, lat, 0, 100, 0, 0),
        ecefPoint(lon, lat, 0, 100, 100, 0), ecefPoint(lon, lat, 0, 0, 100, 0) };
    const auto area = areaResult(square, MeasureAxes::Globe);
    checkTrue("globe area valid", area.valid, area.summary);
    checkNear("globe area 10000", numberIn(area.summary, QStringLiteral("面积 ")),
              10000.0, 1.0);
    checkNear("globe perimeter 400",
              numberIn(area.summary, QStringLiteral("周长 ")), 400.0, 1e-2);
}

} // namespace

int main()
{
    testDistance();
    testCoordinateText();
    testPolyline();
    testArea();
    testAngle();
    testSnapping();
    testGlobe();

    for (const QString& message : failedMessages)
        std::fprintf(stderr, "MEASUREMENT_MATH_FAILED %s\n", message.toUtf8().constData());
    std::fprintf(stderr, "MEASUREMENT_MATH checks=%d failures=%d\n", checks, failures);
    if (failures != 0)
        return 1;
    std::fprintf(stderr, "MEASUREMENT_MATH_OK checks=%d\n", checks);
    return 0;
}
