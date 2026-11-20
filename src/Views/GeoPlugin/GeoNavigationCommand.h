#pragma once

#include <ICommand.h>

#include <QString>
#include <array>

namespace RF {
namespace PluginEngine {
class IApplication;
}
} // namespace RF

namespace View3DGeo {

enum class NavTool { Coordinate, GeoSearch, FlyTo };

// 坐标定位 / 地名搜索 / 飞行：复用 IViewportCamera::setCameraState 做平滑 FlyTo。
class GeoNavigationCommand final : public RF::PluginEngine::ICommand {
  public:
    GeoNavigationCommand(QString name, QString caption, NavTool tool);

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("navigation"); }
    bool getChecked() const override { return false; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return {}; }
    bool getEnabled() const override;
    void onClick() override;
    void onCreate(RF::PluginEngine::IApplication* application) override;
    void onDestroy() override { m_application = nullptr; }

  private:
    void flyTo(const std::array<double, 3>& targetLocal, double viewDistance);
    void geoSearch();

    QString m_name;
    QString m_caption;
    NavTool m_tool;
    RF::PluginEngine::IApplication* m_application = nullptr;
};

} // namespace View3DGeo
