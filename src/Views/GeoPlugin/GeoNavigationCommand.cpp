#include "GeoNavigationCommand.h"

#include <GlobeMath.h>
#include <IApplication.h>
#include <IGeoReference.h>
#include <IPluginContainer.h>
#include <IRenderScene.h>
#include <IViewportLayout.h>
#include <View3DObjectIds.h>

#include <QEventLoop>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVector3D>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace View3DGeo {

namespace {
constexpr double kPi = 3.141592653589793;

std::array<double, 3> normalize(const std::array<double, 3>& v)
{
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len <= 0.0)
        return {0.0, 0.0, 0.0};
    return {v[0] / len, v[1] / len, v[2] / len};
}
} // namespace

GeoNavigationCommand::GeoNavigationCommand(QString name, QString caption, NavTool tool)
    : m_name(std::move(name)), m_caption(std::move(caption)), m_tool(tool)
{
}

bool GeoNavigationCommand::getEnabled() const
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* ref = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    auto* layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    return ref != nullptr && layout != nullptr;
}

void GeoNavigationCommand::onCreate(RF::PluginEngine::IApplication* application)
{
    m_application = application;
}

void GeoNavigationCommand::flyTo(const std::array<double, 3>& targetLocal,
                                 double viewDistance)
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto* view = layout ? layout->activeView() : nullptr;
    if (!view || viewDistance <= 0.0)
        return;

    View3D::ViewportCameraState from;
    view->cameraState(&from);

    // 球面模式下 targetLocal 已是 ECEF：退回目标处法线的南侧上方 45°，
    // 与平面分支的 (0,1,-1) 俯视姿态对应。
    const bool globe = view->globeModeEnabled();
    std::array<double, 3> position {};
    if (globe) {
        using namespace View3D::GlobeMath;
        const Vec3 normal = ecefNormal(targetLocal);
        Vec3 east = normalized(cross({0.0, 0.0, 1.0}, normal));
        if (length(east) < 1e-6)
            east = {1.0, 0.0, 0.0};
        const Vec3 north = normalized(cross(normal, east));
        const double k = std::sqrt(0.5);
        const Vec3 back = add(scale(normal, k), scale(north, -k));
        position = add(targetLocal, scale(back, viewDistance));
    } else {
        // 退到目标后上方 45° 俯视：position = target + d * normalize(0,1,-1)
        const std::array<double, 3> back = normalize({0.0, 1.0, -1.0});
        position = {targetLocal[0] + back[0] * viewDistance,
                    targetLocal[1] + back[1] * viewDistance,
                    targetLocal[2] + back[2] * viewDistance};
    }
    const std::array<double, 3> direction = normalize(
        {targetLocal[0] - position[0], targetLocal[1] - position[1],
         targetLocal[2] - position[2]});

    View3D::ViewportCameraState to = from;
    to.position = position;
    to.target = targetLocal;
    to.direction = direction;
    to.up = globe ? View3D::GlobeMath::ecefNormal(position)
                  : std::array<double, 3> {0.0, 1.0, 0.0};

    // 视口接口不是 QObject，动画宿主取视口的 QWidget：定时器随视口控件一起销毁。
    QWidget* viewHost = view->widget();
    if (!viewHost)
        return;
    const int frames = 60;
    int i = 0;
    auto* timer = new QTimer(viewHost);
    timer->setInterval(16);
    QObject::connect(timer, &QTimer::timeout, viewHost, [=]() mutable {
        const double k = static_cast<double>(++i) / frames;
        const double e = k < 0.5 ? 2.0 * k * k : 1.0 - std::pow(-2.0 * k + 2.0, 2.0) / 2.0;
        View3D::ViewportCameraState s = from;
        for (int c = 0; c < 3; ++c) {
            s.position[c] = from.position[c] + (to.position[c] - from.position[c]) * e;
            s.target[c] = from.target[c] + (to.target[c] - from.target[c]) * e;
        }
        s.direction = normalize({s.target[0] - s.position[0], s.target[1] - s.position[1],
                                 s.target[2] - s.position[2]});
        s.up = globe ? View3D::GlobeMath::ecefNormal(s.position)
                     : std::array<double, 3> {0.0, 1.0, 0.0};
        view->setCameraState(s);
        if (i >= frames) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

void GeoNavigationCommand::geoSearch()
{
    bool ok = false;
    const QString query = QInputDialog::getText(
        nullptr, QStringLiteral("地名搜索"),
        QStringLiteral("输入地名（如 北京、上海浦东、Eiffel Tower）"), QLineEdit::Normal,
        QStringLiteral("北京"), &ok);
    if (!ok || query.isEmpty())
        return;

    // 在线 Geocoding：Nominatim（OpenStreetMap，免 API key）。
    // 注意其使用政策：需设置 User-Agent、限速（本 MVP 为单次请求，够用）。
    // 若要国内地名更稳，可把 base URL 换成天地图（需 key）或高德（需 key）。
    QNetworkAccessManager nam;
    QUrl url(QStringLiteral("https://nominatim.openstreetmap.org/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    q.addQueryItem(QStringLiteral("accept-language"), QStringLiteral("zh-CN"));
    url.setQuery(q);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("View3D/1.0 (geo-plugin geocoding)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isArray()) {
        QMessageBox::information(nullptr, QStringLiteral("地名搜索"),
                                 QStringLiteral("请求失败或返回无法解析（请检查网络）"));
        return;
    }
    const QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("地名搜索"),
                                 QStringLiteral("未找到匹配地点"));
        return;
    }

    struct Candidate {
        QString name;
        double lon = 0.0;
        double lat = 0.0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const double lat = o.value(QStringLiteral("lat")).toString().toDouble();
        const double lon = o.value(QStringLiteral("lon")).toString().toDouble();
        if (std::isfinite(lat) && std::isfinite(lon)) {
            Candidate c;
            c.lat = lat;
            c.lon = lon;
            c.name = o.value(QStringLiteral("display_name")).toString();
            candidates.push_back(c);
        }
    }
    if (candidates.empty()) {
        QMessageBox::information(nullptr, QStringLiteral("地名搜索"),
                                 QStringLiteral("未找到匹配地点"));
        return;
    }

    double lon = candidates[0].lon;
    double lat = candidates[0].lat;
    QString picked = candidates[0].name;
    if (candidates.size() > 1) {
        QStringList items;
        items.reserve(static_cast<int>(candidates.size()));
        for (const auto& c : candidates)
            items.append(c.name);
        bool ok2 = false;
        const QString chosen = QInputDialog::getItem(
            nullptr, QStringLiteral("地名搜索"), QStringLiteral("找到多个匹配，请选择："),
            items, 0, false, &ok2);
        if (!ok2)
            return;
        const int idx = items.indexOf(chosen);
        if (idx < 0)
            return;
        lon = candidates[static_cast<std::size_t>(idx)].lon;
        lat = candidates[static_cast<std::size_t>(idx)].lat;
        picked = candidates[static_cast<std::size_t>(idx)].name;
    }

    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* ref = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    auto* layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!ref)
        return;
    std::array<double, 3> local {};
    // 球面模式直接给 ECEF；平面模式走显示参照系下的局部坐标。
    if (layout && layout->globeModeEnabled()) {
        local = View3D::GlobeMath::geodeticToEcef(lon, lat, 0.0);
    } else if (!ref->lonLatHeightToLocal(lon, lat, 0.0, &local)) {
        return;
    }
    // 默认俯视距离 5km；若地理原点尚未初始化（未加载影像/地形），建议先加载以建立参考。
    flyTo(local, 5000.0);
    QMessageBox::information(
        nullptr, QStringLiteral("地名搜索"),
        QStringLiteral("已定位：%1\n经度 %2，纬度 %3")
            .arg(picked)
            .arg(lon, 0, 'f', 5)
            .arg(lat, 0, 'f', 5));
}

void GeoNavigationCommand::onClick()
{
    auto* container = m_application ? m_application->getPluginContainer() : nullptr;
    auto* ref = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    auto* layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!ref || !layout)
        return;

    if (m_tool == NavTool::Coordinate) {
        bool ok = false;
        const QString text = QInputDialog::getText(
            nullptr, QStringLiteral("坐标定位"),
            QStringLiteral("经度,纬度,高程(米),视角距离(米)"), QLineEdit::Normal,
            QStringLiteral("116.39,39.91,0,2000"), &ok);
        if (!ok)
            return;
        const QStringList parts = text.split(QStringLiteral(","), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            return;
        const double longitude = parts[0].toDouble();
        const double latitude = parts[1].toDouble();
        const double height = parts.size() > 2 ? parts[2].toDouble() : 0.0;
        const double distance = parts.size() > 3 ? parts[3].toDouble() : 2000.0;
        std::array<double, 3> local {};
        if (layout->globeModeEnabled()) {
            local = View3D::GlobeMath::geodeticToEcef(longitude, latitude, height);
        } else if (!ref->lonLatHeightToLocal(longitude, latitude, height, &local)) {
            return;
        }
        flyTo(local, distance);
    } else if (m_tool == NavTool::FlyTo) {
        // 飞到当前场景可见范围中心（MVP）；后续改为飞到选中图层/要素。
        auto* scene = layout->scene();
        View3D::RenderBounds bounds {};
        if (scene && scene->visibleBounds(&bounds) && bounds.valid) {
            std::array<double, 3> center = {
                (bounds.minimum[0] + bounds.maximum[0]) / 2.0,
                (bounds.minimum[1] + bounds.maximum[1]) / 2.0,
                (bounds.minimum[2] + bounds.maximum[2]) / 2.0};
            if (layout->globeModeEnabled()) {
                // 球面下可见包围体在渲染空间（ECEF − 锚点），加回锚点才是 ECEF。
                std::array<double, 3> anchor {};
                if (scene->globeRenderOrigin(&anchor))
                    for (int axis = 0; axis < 3; ++axis)
                        center[axis] += anchor[axis];
            }
            const double span = std::max({bounds.maximum[0] - bounds.minimum[0],
                                         bounds.maximum[1] - bounds.minimum[1],
                                         bounds.maximum[2] - bounds.minimum[2]});
            flyTo(center, std::max(span, 1000.0));
        }
    } else {
        // GeoSearch：接入在线 Geocoding（Nominatim）。
        geoSearch();
    }
}

} // namespace View3DGeo
