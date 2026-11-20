#pragma once

#include <array>
#include <cmath>

namespace View3D {

// WGS84 椭球数学：球面模式的渲染端、数据插件与视图编排共用。
// 参数与 crs 插件、cesium-native 一致；全程双精度，只在渲染提交最后一步转 float。
namespace GlobeMath {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSemiMajorAxis = 6378137.0;
constexpr double kFlattening = 1.0 / 298.257223563;
constexpr double kSemiMinorAxis = kSemiMajorAxis * (1.0 - kFlattening);
constexpr double kEccentricitySquared = kFlattening * (2.0 - kFlattening);
constexpr double kSecondEccentricitySquared =
    kEccentricitySquared / (1.0 - kEccentricitySquared);
constexpr double kDegreesPerRadian = 180.0 / kPi;
constexpr double kRadiansPerDegree = kPi / 180.0;

using Vec3 = std::array<double, 3>;

inline Vec3 add(const Vec3& a, const Vec3& b)
{
    return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
}

inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

inline Vec3 scale(const Vec3& a, double s)
{
    return { a[0] * s, a[1] * s, a[2] * s };
}

inline double dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1],
             a[2] * b[0] - a[0] * b[2],
             a[0] * b[1] - a[1] * b[0] };
}

inline double length(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

inline Vec3 normalized(const Vec3& a)
{
    const double len = length(a);
    return len > 1e-12 ? scale(a, 1.0 / len) : Vec3 { 0.0, 0.0, 0.0 };
}

inline Vec3 geodeticToEcef(double lonDeg, double latDeg, double heightMeters)
{
    const double lon = lonDeg * kRadiansPerDegree;
    const double lat = latDeg * kRadiansPerDegree;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double primeVertical =
        kSemiMajorAxis / std::sqrt(1.0 - kEccentricitySquared * sinLat * sinLat);
    return { (primeVertical + heightMeters) * cosLat * std::cos(lon),
             (primeVertical + heightMeters) * cosLat * std::sin(lon),
             (primeVertical * (1.0 - kEccentricitySquared) + heightMeters) * sinLat };
}

// Bowring 闭式解（与 crs 插件同款）：地球尺度毫米级误差，无需迭代。
inline bool ecefToGeodetic(const Vec3& ecef, double* lonDeg, double* latDeg,
                           double* heightMeters)
{
    const double x = ecef[0];
    const double y = ecef[1];
    const double z = ecef[2];
    const double p = std::hypot(x, y);
    if (!std::isfinite(p) || !std::isfinite(z))
        return false;
    *lonDeg = (p < 1e-9) ? 0.0 : std::atan2(y, x) * kDegreesPerRadian;
    const double theta = std::atan2(z, (1.0 - kFlattening) * p);
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);
    const double numerator = z + kSecondEccentricitySquared * kSemiMinorAxis *
        sinTheta * sinTheta * sinTheta;
    const double denominator =
        p - kEccentricitySquared * kSemiMajorAxis * cosTheta * cosTheta * cosTheta;
    const double lat = std::atan2(numerator, denominator);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double primeVertical =
        kSemiMajorAxis / std::sqrt(1.0 - kEccentricitySquared * sinLat * sinLat);
    // 接近极点时 p/cos(lat) 会爆掉，改用 z 分支求高程。
    *heightMeters = std::abs(cosLat) > 1e-6
        ? p / cosLat - primeVertical
        : std::abs(z) / std::abs(sinLat) -
              primeVertical * (1.0 - kEccentricitySquared);
    *latDeg = lat * kDegreesPerRadian;
    return std::isfinite(*latDeg) && std::isfinite(*heightMeters);
}

// 给定经纬度处的椭球法线（大地纬度方向，单位向量）。
inline Vec3 geodeticNormal(double lonDeg, double latDeg)
{
    const double lon = lonDeg * kRadiansPerDegree;
    const double lat = latDeg * kRadiansPerDegree;
    return { std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon),
             std::sin(lat) };
}

// ECEF 点处的 up：先归算大地纬度再取法线；归算失败退回径向。
inline Vec3 ecefNormal(const Vec3& ecef)
{
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double height = 0.0;
    if (ecefToGeodetic(ecef, &lonDeg, &latDeg, &height))
        return geodeticNormal(lonDeg, latDeg);
    return normalized(ecef);
}

inline double ecefHeight(const Vec3& ecef)
{
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double height = 0.0;
    if (ecefToGeodetic(ecef, &lonDeg, &latDeg, &height))
        return height;
    return length(ecef) - kSemiMajorAxis;
}

// 射线与 WGS84 椭球的第一个交点。center 是椭球中心在射线坐标系里的表示：
// ECEF 里传 (0,0,0)，渲染空间里传 -renderOrigin。取最近正根，起点在椭球内
// 时取出射点；无交点返回 false。direction 需为单位向量。
inline bool rayEllipsoidHit(const Vec3& origin, const Vec3& direction,
                            const Vec3& center, Vec3* point)
{
    const Vec3 o = sub(origin, center);
    const double invA2 = 1.0 / (kSemiMajorAxis * kSemiMajorAxis);
    const double invB2 = 1.0 / (kSemiMinorAxis * kSemiMinorAxis);
    const double a = (direction[0] * direction[0] + direction[1] * direction[1]) *
            invA2 +
        direction[2] * direction[2] * invB2;
    const double b = 2.0 *
        ((o[0] * direction[0] + o[1] * direction[1]) * invA2 +
         o[2] * direction[2] * invB2);
    const double c =
        (o[0] * o[0] + o[1] * o[1]) * invA2 + o[2] * o[2] * invB2 - 1.0;
    if (a < 1e-20)
        return false;
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0)
        return false;
    const double sqrtDisc = std::sqrt(disc);
    double t = (-b - sqrtDisc) / (2.0 * a);
    if (t <= 0.0)
        t = (-b + sqrtDisc) / (2.0 * a);
    if (t <= 0.0 || !std::isfinite(t))
        return false;
    if (point)
        *point = add(origin, scale(direction, t));
    return true;
}

} // namespace GlobeMath
} // namespace View3D