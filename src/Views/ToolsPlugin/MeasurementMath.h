#pragma once

#include "MeasurementTypes.h"

#include <array>

namespace View3DPlugins {

MeasurePoint snapToGeometry(
    const MeasurePoint& hitPoint,
    const std::array<std::array<double, 3>, 3>& triangleVertices, double tolerance,
    bool enabled, MeasureAxes axes = MeasureAxes::Spatial3D);

MeasureResult coordinateResult(const MeasurePoint& point,
                               MeasureAxes axes = MeasureAxes::Spatial3D);
MeasureResult distanceResult(const MeasurePoint& first, const MeasurePoint& second,
                             MeasureAxes axes = MeasureAxes::Spatial3D);
MeasureResult polylineResult(const QList<MeasurePoint>& points,
                             MeasureAxes axes = MeasureAxes::Spatial3D);
MeasureResult angleResult(const MeasurePoint& first, const MeasurePoint& vertex,
                          const MeasurePoint& third,
                          MeasureAxes axes = MeasureAxes::Spatial3D);
MeasureResult areaResult(const QList<MeasurePoint>& points,
                         MeasureAxes axes = MeasureAxes::Spatial3D);

// 平面读数：屏幕横轴取 X（东），纵轴取 -Z（北），与二维视图的"横 X 纵 Y"一致。
QString formatMeasurePoint(const MeasurePoint& point, MeasureAxes axes);

QString measureToolCaption(MeasureTool tool);
QString measureUsageHint(MeasureTool tool);

} // namespace View3DPlugins
