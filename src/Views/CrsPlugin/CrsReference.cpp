#include "CrsReference.h"

#include <cmath>

namespace View3DCrs {

namespace {

// WGS84 椭球参数（与 3D Tiles 侧 Cesium 使用的椭球一致）。
constexpr double kPi = 3.14159265358979323846;
constexpr double kSemiMajorAxis = 6378137.0;
constexpr double kFlattening = 1.0 / 298.257223563;
constexpr double kEccentricitySquared = kFlattening * (2.0 - kFlattening);
constexpr double kSecondEccentricitySquared =
    kEccentricitySquared / (1.0 - kEccentricitySquared);
constexpr double kDegreesPerRadian = 180.0 / kPi;
constexpr double kRadiansPerDegree = kPi / 180.0;
constexpr const char* kMainCrsText = "EPSG:4979";

// 平面微分的探针步长：0.00001° ≈ 1.1 m，足够小到当导数用，又足够大到不被
// PROJ 的双精度尾数放大噪声。
constexpr double kProbeDegrees = 1.0e-5;

bool looksGeographicText(const QString& crs)
{
    const QString upper = crs.toUpper();
    return upper.contains(QStringLiteral("4326")) || upper.contains(QStringLiteral("4979")) ||
        upper.contains(QStringLiteral("CRS84"));
}

void geodeticToEcef(double lonDeg, double latDeg, double heightMeters,
                    std::array<double, 3>* ecef)
{
    const double lon = lonDeg * kRadiansPerDegree;
    const double lat = latDeg * kRadiansPerDegree;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double primeVertical =
        kSemiMajorAxis / std::sqrt(1.0 - kEccentricitySquared * sinLat * sinLat);
    (*ecef)[0] = (primeVertical + heightMeters) * cosLat * std::cos(lon);
    (*ecef)[1] = (primeVertical + heightMeters) * cosLat * std::sin(lon);
    (*ecef)[2] = (primeVertical * (1.0 - kEccentricitySquared) + heightMeters) * sinLat;
}

// Bowring 闭式解：以归化纬度为参量一次求解，地球尺度误差在毫米级，无需迭代。
bool ecefToGeodetic(const std::array<double, 3>& ecef, double* lonDeg, double* latDeg,
                    double* heightMeters)
{
    const double x = ecef[0];
    const double y = ecef[1];
    const double z = ecef[2];
    const double p = std::hypot(x, y);
    if (!std::isfinite(p) || !std::isfinite(z))
        return false;
    *lonDeg = (p < 1e-9) ? 0.0 : std::atan2(y, x) * kDegreesPerRadian;
    const double semiMinor = kSemiMajorAxis * std::sqrt(1.0 - kEccentricitySquared);
    const double theta = std::atan2(z, (1.0 - kFlattening) * p);
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);
    const double numerator = z + kSecondEccentricitySquared * semiMinor * sinTheta *
        sinTheta * sinTheta;
    const double denominator =
        p - kEccentricitySquared * kSemiMajorAxis * cosTheta * cosTheta * cosTheta;
    const double lat = std::atan2(numerator, denominator);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double primeVertical =
        kSemiMajorAxis / std::sqrt(1.0 - kEccentricitySquared * sinLat * sinLat);
    // 接近极点时 p/cos(lat) 会爆掉，改用 z 分支求高程。
    *heightMeters = std::abs(cosLat) > 1e-6 ? p / cosLat - primeVertical
                                             : std::abs(z) / std::abs(sinLat) -
                primeVertical * (1.0 - kEccentricitySquared);
    *latDeg = lat * kDegreesPerRadian;
    return std::isfinite(*latDeg) && std::isfinite(*heightMeters);
}

// 椭球的卯酉圈/子午圈曲率半径：探针位移要换算成「真北切平面上的米」，
// 用的是局部取向，与世界原点无关。
// 2×2 求逆（行主序 [a00,a01,a10,a11]）；行列式过小判定失败。
bool invert2x2(const std::array<double, 4>& m, std::array<double, 4>* out)
{
    const double det = m[0] * m[3] - m[1] * m[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-18)
        return false;
    *out = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det};
    return true;
}

void primeRadii(double latDeg, double* eastMetersPerRadian, double* northMetersPerRadian)
{
    const double lat = latDeg * kRadiansPerDegree;
    const double sinLat = std::sin(lat);
    const double factor = 1.0 - kEccentricitySquared * sinLat * sinLat;
    const double primeVertical = kSemiMajorAxis / std::sqrt(factor);
    const double meridian = kSemiMajorAxis * (1.0 - kEccentricitySquared) /
        (factor * std::sqrt(factor));
    *eastMetersPerRadian = primeVertical * std::cos(lat);
    *northMetersPerRadian = meridian;
}

} // namespace

CrsReference::CrsReference()
{
    m_backend.initialize(QString());
}

void CrsReference::setProjDataDirectory(const QString& directory)
{
    m_backend.initialize(directory);
}

bool CrsReference::ready() const noexcept
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_ready;
}

std::vector<CrsReference::GeoFrameCallback> CrsReference::listenersLocked() const
{
    std::vector<GeoFrameCallback> listeners;
    listeners.reserve(m_listeners.size());
    for (const auto& [token, callback] : m_listeners) {
        static_cast<void>(token);
        listeners.push_back(callback);
    }
    return listeners;
}

QString CrsReference::displayCrsTextLocked() const
{
    // 开关关着时生效的就是 ENU 显示；记着的投影系留到下次打开再用。
    if (!m_projectionEnabled || m_display.crs.isEmpty())
        return QStringLiteral("EPSG:4979");
    return m_display.crs;
}

bool CrsReference::setOrigin(double lonDeg, double latDeg, double heightMeters)
{
    if (!std::isfinite(lonDeg) || !std::isfinite(latDeg) || !std::isfinite(heightMeters) ||
        std::abs(latDeg) > 90.0 || std::abs(lonDeg) > 360.0)
        return false;
    std::vector<GeoFrameCallback> listeners;
    QString displayText;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_ready)
            return false;   // 已钉住：不搬动原点，避免已提交的网格整体漂移
        Frame frame;
        frame.lonDeg = lonDeg;
        frame.latDeg = latDeg;
        frame.heightMeters = heightMeters;
        geodeticToEcef(lonDeg, latDeg, heightMeters, &frame.ecef);
        const double lat = latDeg * kRadiansPerDegree;
        const double lon = lonDeg * kRadiansPerDegree;
        frame.sinLat = std::sin(lat);
        frame.cosLat = std::cos(lat);
        frame.sinLon = std::sin(lon);
        frame.cosLon = std::cos(lon);
        m_frame = frame;
        m_ready = true;
        // 投影显示的局部零点对应原点在投影面上的像，原点定了才有意义。
        refreshDisplayLocked();
        listeners = listenersLocked();
        displayText = displayCrsTextLocked();
    }
    // 只按整体矩阵摆放的流式数据（倾斜摄影）要等原点钉上才知道自己该待哪儿。
    for (const auto& callback : listeners)
        callback(GeoFrameChange::WorldOrigin, displayText);
    return true;
}

void CrsReference::resetOrigin()
{
    std::vector<GeoFrameCallback> listeners;
    QString displayText;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_ready = false;
        m_frame = Frame();
        m_display.valid = false;
        m_display.originGrid = { {0.0, 0.0} };
        listeners = listenersLocked();
        displayText = displayCrsTextLocked();
    }
    for (const auto& callback : listeners)
        callback(GeoFrameChange::WorldOrigin, displayText);
}

bool CrsReference::setOriginFromEcef(const std::array<double, 3>& ecefMeters)
{
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double heightMeters = 0.0;
    if (!ecefToGeodetic(ecefMeters, &lonDeg, &latDeg, &heightMeters))
        return false;
    return setOrigin(lonDeg, latDeg, heightMeters);
}

void CrsReference::enuOf(double lonDeg, double latDeg, double heightMeters,
                         std::array<double, 3>* eastNorthUp) const
{
    std::array<double, 3> ecef {};
    geodeticToEcef(lonDeg, latDeg, heightMeters, &ecef);
    const Frame& frame = m_frame;
    const double dx = ecef[0] - frame.ecef[0];
    const double dy = ecef[1] - frame.ecef[1];
    const double dz = ecef[2] - frame.ecef[2];
    (*eastNorthUp)[0] = -frame.sinLon * dx + frame.cosLon * dy;
    (*eastNorthUp)[1] = -frame.sinLat * frame.cosLon * dx - frame.sinLat * frame.sinLon * dy +
        frame.cosLat * dz;
    (*eastNorthUp)[2] = frame.cosLat * frame.cosLon * dx + frame.cosLat * frame.sinLon * dy +
        frame.sinLat * dz;
}

bool CrsReference::frameOf(const QString& crs, double lonDeg, double latDeg,
                           double heightMeters, PlaneFrame* frame) const
{
    if (!frame)
        return false;
    *frame = PlaneFrame {};
    const QString trimmed = crs.trimmed();
    if (trimmed.isEmpty() || looksGeographicText(trimmed))
        return true;   // 地理系没有自己的网格轴，取真北切平面：单位阵
    bool projected = false;
    if (!m_backend.isDisplayable(trimmed, &projected))
        return false;
    if (!projected)
        return true;

    // 三点差分求「真(东,北)米 -> 网格(东,北)米」：原点 + 真东一步 + 真北一步。
    std::vector<double> probe {lonDeg, latDeg, heightMeters,
                               lonDeg + kProbeDegrees, latDeg, heightMeters,
                               lonDeg, latDeg + kProbeDegrees, heightMeters};
    if (!m_backend.transformBetween(QString::fromLatin1(kMainCrsText), trimmed, probe.data(), 3))
        return false;
    double eastPerRadian = 0.0;
    double northPerRadian = 0.0;
    primeRadii(latDeg, &eastPerRadian, &northPerRadian);
    const double eastMeters = eastPerRadian * kProbeDegrees * kRadiansPerDegree;
    const double northMeters = northPerRadian * kProbeDegrees * kRadiansPerDegree;
    if (!(eastMeters > 0.0) || !(northMeters > 0.0))
        return false;
    // 列向量分别是「真东一步」「真北一步」在网格轴上的位移；行主序存 a00,a01,a10,a11。
    frame->trueToGrid[0] = (probe[3] - probe[0]) / eastMeters;
    frame->trueToGrid[1] = (probe[6] - probe[0]) / northMeters;
    frame->trueToGrid[2] = (probe[4] - probe[1]) / eastMeters;
    frame->trueToGrid[3] = (probe[7] - probe[1]) / northMeters;
    const double det =
        frame->trueToGrid[0] * frame->trueToGrid[3] - frame->trueToGrid[1] * frame->trueToGrid[2];
    if (!(std::abs(det) > 1e-12))
        return false;
    frame->scaleFactor = std::sqrt(std::abs(det));
    // 正 = 网格北落在真北以东（中央经线以东为正，与经典收敛角定义一致）。
    frame->convergenceDeg =
        std::atan2(frame->trueToGrid[2], frame->trueToGrid[0]) * kDegreesPerRadian;
    return true;
}

void CrsReference::refreshDisplayLocked()
{
    m_display.valid = false;
    m_display.originGrid = { {0.0, 0.0} };
    m_display.frame = PlaneFrame {};
    if (!projectionActiveLocked() || !m_ready)
        return;
    std::array<double, 3> originProbe {m_frame.lonDeg, m_frame.latDeg, m_frame.heightMeters};
    if (!m_backend.transformBetween(QString::fromLatin1(kMainCrsText), m_display.crs,
                                    originProbe.data(), 1)) {
        m_lastError = QStringLiteral("世界原点无法投影到显示参照系 %1：%2")
                          .arg(m_display.crs.left(40), m_backend.lastError());
        return;
    }
    m_display.originGrid = {originProbe[0], originProbe[1]};
    if (!frameOf(m_display.crs, m_frame.lonDeg, m_frame.latDeg, m_frame.heightMeters,
                 &m_display.frame)) {
        m_lastError = QStringLiteral("显示参照系 %1 的原点微分求不出来").arg(m_display.crs.left(40));
        return;
    }
    m_display.valid = true;
}

bool CrsReference::lonLatHeightToLocal(double lonDeg, double latDeg, double heightMeters,
                                       std::array<double, 3>* localXyz) const
{
    if (!localXyz)
        return false;
    if (!std::isfinite(lonDeg) || !std::isfinite(latDeg) || !std::isfinite(heightMeters))
        return false;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_ready)
        return false;
    if (!projectionActiveLocked()) {
        std::array<double, 3> enu {};
        enuOf(lonDeg, latDeg, heightMeters, &enu);
        (*localXyz)[0] = enu[0];
        (*localXyz)[1] = enu[2];
        (*localXyz)[2] = -enu[1];   // View3D 约定：+Z 朝南，屏幕向上是北
    } else {
        if (!m_display.valid)
            return false;
        std::array<double, 3> grid {lonDeg, latDeg, heightMeters};
        if (!m_backend.transformBetween(QString::fromLatin1(kMainCrsText), m_display.crs,
                                        grid.data(), 1))
            return false;
        // 高程一律用椭球高：投影第三轴可能是别的垂直基准，拿它当 up 会整层抬升。
        (*localXyz)[0] = grid[0] - m_display.originGrid[0];
        (*localXyz)[1] = heightMeters;
        (*localXyz)[2] = -(grid[1] - m_display.originGrid[1]);
    }
    return std::isfinite((*localXyz)[0]) && std::isfinite((*localXyz)[1]) &&
        std::isfinite((*localXyz)[2]);
}

bool CrsReference::localToLonLatHeight(const std::array<double, 3>& localXyz, double* lonDeg,
                                       double* latDeg, double* heightMeters) const
{
    if (!lonDeg || !latDeg || !heightMeters)
        return false;
    if (!std::isfinite(localXyz[0]) || !std::isfinite(localXyz[1]) ||
        !std::isfinite(localXyz[2]))
        return false;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_ready)
        return false;
    if (!projectionActiveLocked()) {
        const Frame& frame = m_frame;
        const double east = localXyz[0];
        const double up = localXyz[1];
        const double north = -localXyz[2];
        std::array<double, 3> delta {
            { -frame.sinLon * east - frame.sinLat * frame.cosLon * north +
                  frame.cosLat * frame.cosLon * up,
              frame.cosLon * east - frame.sinLat * frame.sinLon * north +
                  frame.cosLat * frame.sinLon * up,
              frame.cosLat * north + frame.sinLat * up }
        };
        const std::array<double, 3> ecef { { frame.ecef[0] + delta[0], frame.ecef[1] + delta[1],
                                             frame.ecef[2] + delta[2] } };
        return ecefToGeodetic(ecef, lonDeg, latDeg, heightMeters);
    }
    if (!m_display.valid)
        return false;
    std::array<double, 3> grid {m_display.originGrid[0] + localXyz[0],
                                 m_display.originGrid[1] - localXyz[2], localXyz[1]};
    if (!m_backend.transformBetween(m_display.crs, QString::fromLatin1(kMainCrsText),
                                    grid.data(), 1))
        return false;
    *lonDeg = grid[0];
    *latDeg = grid[1];
    *heightMeters = grid[2];
    return std::isfinite(*lonDeg) && std::isfinite(*latDeg) && std::isfinite(*heightMeters);
}

QString CrsReference::horizontalCrs() const
{
    return QStringLiteral("EPSG:4326");
}

QString CrsReference::mainCrs() const
{
    return QStringLiteral("EPSG:4979");
}

QString CrsReference::displayCrs() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return displayCrsTextLocked();
}

bool CrsReference::dynamicProjectionEnabled() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_projectionEnabled;
}

bool CrsReference::setDynamicProjectionEnabled(bool enabled)
{
    std::vector<GeoFrameCallback> listeners;
    QString current;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_projectionEnabled == enabled)
            return true;   // 没变化：不重放，也不发通知
        m_projectionEnabled = enabled;
        refreshDisplayLocked();
        listeners = listenersLocked();
        current = displayCrsTextLocked();
    }
    // 关到开要按记住的参照系重放与跟随，开到关要一并复位成 ENU 摆放。
    for (const auto& callback : listeners)
        callback(GeoFrameChange::DisplayCrs, current);
    return true;
}

bool CrsReference::setDisplayCrs(const QString& crs)
{
    const QString trimmed = crs.trimmed();
    bool projected = false;
    if (!trimmed.isEmpty() && !looksGeographicText(trimmed)) {
        if (!m_backend.isDisplayable(trimmed, &projected)) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_lastError = QStringLiteral("显示参照系不可用：%1（%2）")
                              .arg(trimmed.left(40), m_backend.lastError());
            return false;
        }
    }
    std::vector<GeoFrameCallback> listeners;
    QString current;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_projectionEnabled && m_display.crs == trimmed && m_display.projected == projected)
            return true;   // 没变化：不重放，也不发通知
        m_display.crs = trimmed;
        m_display.projected = projected;
        // 显式指定参照系就等于打开动态投影开关，关它要用「不启用」。
        m_projectionEnabled = true;
        refreshDisplayLocked();
        if (projectionActiveLocked() && !m_display.valid) {
            // 已选投影但原点还没钉住：不算失败，第一个地理数据源进来时会补齐。
            m_lastError = QStringLiteral("显示参照系 %1 已记录，等待世界原点").arg(trimmed.left(40));
        }
        listeners = listenersLocked();
        current = displayCrsTextLocked();
    }
    // 回调里可能回头查 displayCrs()，所以必须解锁后再调。
    for (const auto& callback : listeners)
        callback(GeoFrameChange::DisplayCrs, current);
    return true;
}

QString CrsReference::lastError() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_lastError;
}

bool CrsReference::planeFrameAt(const QString& crs, double lonDeg, double latDeg,
                                double heightMeters, PlaneFrame* frame) const
{
    if (!std::isfinite(lonDeg) || !std::isfinite(latDeg) || !std::isfinite(heightMeters))
        return false;
    return frameOf(crs, lonDeg, latDeg, heightMeters, frame);
}

bool CrsReference::planePlacement(const QString& sourceCrs, double sourceOriginLonDeg,
                                  double sourceOriginLatDeg, double sourceOriginHeightMeters,
                                  PlanePlacement* placement) const
{
    if (!placement)
        return false;
    *placement = PlanePlacement {};
    QString displayText;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        // 开关关着时保持改动前的摆放：不转、不缩放，也不按地理位置挪动。
        if (!m_projectionEnabled)
            return true;
        displayText = m_display.crs;
    }
    PlaneFrame sourceFrame;
    if (!frameOf(sourceCrs, sourceOriginLonDeg, sourceOriginLatDeg, sourceOriginHeightMeters,
                 &sourceFrame))
        return false;
    std::array<double, 4> sourceToTrue {};
    if (!invert2x2(sourceFrame.trueToGrid, &sourceToTrue))
        return false;
    PlaneFrame displayFrame;
    if (!frameOf(displayText, sourceOriginLonDeg, sourceOriginLatDeg,
                 sourceOriginHeightMeters, &displayFrame))
        return false;
    // D = 显示网格 ∘ 源网格⁻¹：共形投影下就是「收敛角差 + 比例因子」的相似变换。
    const std::array<double, 4>& a = displayFrame.trueToGrid;
    const double d00 = a[0] * sourceToTrue[0] + a[1] * sourceToTrue[2];
    const double d01 = a[0] * sourceToTrue[1] + a[1] * sourceToTrue[3];
    const double d10 = a[2] * sourceToTrue[0] + a[3] * sourceToTrue[2];
    const double d11 = a[2] * sourceToTrue[1] + a[3] * sourceToTrue[3];
    const double det = d00 * d11 - d01 * d10;
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return false;
    placement->yawDegrees = std::atan2(d10, d00) * kDegreesPerRadian;
    placement->horizontalScale = std::sqrt(std::abs(det));
    // 源局部原点在当前显示下的世界位置；原点还没钉时留在 (0,0,0)，与既有行为一致。
    std::array<double, 3> local { {0.0, 0.0, 0.0} };
    if (lonLatHeightToLocal(sourceOriginLonDeg, sourceOriginLatDeg, sourceOriginHeightMeters,
                            &local))
        placement->originLocal = local;
    return true;
}

bool CrsReference::sourcePointsToLocal(const QString& sourceCrs, const double* xyz,
                                       std::size_t pointCount,
                                       std::vector<double>* localXyz) const
{
    if (!xyz || !localXyz || pointCount == 0)
        return false;
    std::vector<double> buffer(xyz, xyz + pointCount * 3);
    QString displayCrsText;
    bool projected = false;
    std::array<double, 2> originGrid { {0.0, 0.0} };
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_ready)
            return false;
        projected = projectionActiveLocked();
        displayCrsText = projected ? m_display.crs : QString();
        originGrid = m_display.originGrid;
        if (projected && !m_display.valid)
            return false;
    }
    const QString source = sourceCrs.trimmed().isEmpty()
        ? QString::fromLatin1(kMainCrsText)
        : sourceCrs.trimmed();
    const QString target = projected ? displayCrsText : QString::fromLatin1(kMainCrsText);
    // 投影显示时源→网格一步到位，省掉「先回经纬度再投影」的第二次 PROJ 采样。
    if (source != target &&
        !m_backend.transformBetween(source, target, buffer.data(), pointCount))
        return false;
    localXyz->clear();
    localXyz->reserve(pointCount * 3);
    for (std::size_t i = 0; i < pointCount; ++i) {
        const double* in = buffer.data() + i * 3;
        if (projected) {
            localXyz->push_back(in[0] - originGrid[0]);
            localXyz->push_back(in[2]);
            localXyz->push_back(-(in[1] - originGrid[1]));
            continue;
        }
        std::array<double, 3> local {};
        if (!lonLatHeightToLocal(in[0], in[1], in[2], &local))
            return false;
        localXyz->push_back(local[0]);
        localXyz->push_back(local[1]);
        localXyz->push_back(local[2]);
    }
    for (double value : *localXyz) {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

std::uint64_t CrsReference::addGeoFrameChangeListener(
    std::function<void(GeoFrameChange change, const QString& displayCrs)> callback)
{
    if (!callback)
        return 0;
    std::lock_guard<std::mutex> guard(m_mutex);
    m_listeners[++m_listenerToken] = std::move(callback);
    return m_listenerToken;
}

void CrsReference::removeGeoFrameChangeListener(std::uint64_t token)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_listeners.erase(token);
}

bool CrsReference::crsInfo(const QString& sourceCrs, CrsInfo* info) const
{
    return m_backend.describe(sourceCrs, info);
}

bool CrsReference::transformToMain(const QString& sourceCrs, double* xyz,
                                   std::size_t pointCount) const
{
    return m_backend.transformToMain(sourceCrs, xyz, pointCount);
}

bool CrsReference::origin(double* lonDeg, double* latDeg, double* heightMeters) const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_ready)
        return false;
    if (lonDeg)
        *lonDeg = m_frame.lonDeg;
    if (latDeg)
        *latDeg = m_frame.latDeg;
    if (heightMeters)
        *heightMeters = m_frame.heightMeters;
    return true;
}

bool CrsReference::originEcef(std::array<double, 3>* ecefMeters) const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_ready || !ecefMeters)
        return false;
    *ecefMeters = m_frame.ecef;
    return true;
}

bool CrsReference::localToEcefPoint(const std::array<double, 3>& localXyz,
                                    std::array<double, 3>* ecefMeters) const
{
    if (!ecefMeters)
        return false;
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double heightMeters = 0.0;
    if (!localToLonLatHeight(localXyz, &lonDeg, &latDeg, &heightMeters))
        return false;
    geodeticToEcef(lonDeg, latDeg, heightMeters, ecefMeters);
    return true;
}

bool CrsReference::ecefToLocalPoint(const std::array<double, 3>& ecefMeters,
                                    std::array<double, 3>* localXyz) const
{
    double lonDeg = 0.0;
    double latDeg = 0.0;
    double heightMeters = 0.0;
    if (!ecefToGeodetic(ecefMeters, &lonDeg, &latDeg, &heightMeters))
        return false;
    return lonLatHeightToLocal(lonDeg, latDeg, heightMeters, localXyz);
}

} // namespace View3DCrs
