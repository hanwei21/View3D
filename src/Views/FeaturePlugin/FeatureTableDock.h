#pragma once

#include <IDockWindow.h>

#include <QVariantMap>

#include <cstdint>

class QLabel;
class QTableWidget;
class QWidget;

namespace View3DFeature {

// 一次要素选中的展示载荷：图层名与要素总数由图层/网格侧解析，
// 属性字段来自网格要素表（矢量图层、3D Tiles batch table 等都是这个来源）。
struct FeatureSelection {
    QString layerName;
    QString sourceKey;
    std::int64_t featureId = -1;
    std::int64_t tableFeatureCount = -1;
    bool tableFeatureCountValid = false;
    QVariantMap attributes;
};

// 要素属性表：显示最近一次拾取到的要素字段与值，跨数据源通用。
class FeatureTableDock final : public RF::PluginEngine::IDockWindow {
  public:
    FeatureTableDock(QString name, QString caption);

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Bottom;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}
    QWidget* getWidget() const override { return m_widget; }

    void showFeature(const FeatureSelection& selection);
    void clearSelection();

  private:
    QString m_name;
    QString m_caption;
    QWidget* m_widget = nullptr;
    QLabel* m_title = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace View3DFeature
