#pragma once

#include <CoreTypes.h>
#include <IDockWindow.h>
#include <ILayerPropertiesDock.h>
#include <IServicePublisher.h>

#include <QString>

#include <array>
#include <memory>
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class QWidget;

namespace View3D {
class ILayer;
} // namespace View3D

namespace View3DPlugins {

class SceneLayerService;
class SceneTreeCoordinator;

// 图层属性面板：编辑当前选中图层的通用属性（可见性、锁定、透明度、位置偏移、
// 显示距离、色调/边线/亮度/对比度），并回显来源、加载状态与统计。
// 只走 ISceneLayerService 与 ILayer 的通用接口：不认识倾斜摄影、3D Tiles 等
// 任何具体数据格式，可编辑范围按服务端真实能力禁用，不放假开关。
class LayerPropertiesDock final : public RF::PluginEngine::IDockWindow,
                                  public RF::PluginEngine::IServicePublisher,
                                  public View3D::ILayerPropertiesDock {
  public:
    LayerPropertiesDock(QString name, QString caption);
    ~LayerPropertiesDock() override;

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Right;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override;
    QWidget* getWidget() const override { return m_widget; }

    QList<RF::PluginEngine::PublishedService> publishedServices() const override;

    // dock 由配置构建阶段创建，图层服务要到场景扩展初始化时才存在，
    // 因此接线交给场景扩展，由它在拿到服务后调一次。
    void bind(SceneLayerService& service, SceneTreeCoordinator& coordinator);

    // ILayerPropertiesDock：外部（含冒烟检查）以此驱动面板，不必认识本类。
    View3D::LayerId currentLayer() const override { return m_current; }
    void setCurrentLayer(const View3D::LayerId& layerId) override;

  private:
    void reload();
    void commitName();
    void commitVisible();
    void commitLocked();
    void commitOpacity();
    void commitOffset();
    void commitDistance();
    void commitStyle();
    void setEditable(bool layerEditable, bool distanceEditable, bool styleEditable);
    std::shared_ptr<View3D::ILayer> currentLayerPtr() const;

    QString m_name;
    QString m_caption;
    QWidget* m_widget = nullptr;
    SceneLayerService* m_service = nullptr;
    SceneTreeCoordinator* m_coordinator = nullptr;
    // dock 与场景扩展同生共死，但回调注册在服务/协调器上；用一个存活标志
    // 让已析构的面板不再被回调命中。
    std::shared_ptr<bool> m_alive;

    QLabel* m_hint = nullptr;
    QLabel* m_typeLabel = nullptr;
    QLabel* m_stateLabel = nullptr;
    QLabel* m_sourceLabel = nullptr;
    QLabel* m_crsLabel = nullptr;
    QLabel* m_referenceFrameLabel = nullptr;
    QLabel* m_statsLabel = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QCheckBox* m_visibleCheck = nullptr;
    QCheckBox* m_lockedCheck = nullptr;
    QSlider* m_opacity = nullptr;
    QLabel* m_opacityValue = nullptr;
    QDoubleSpinBox* m_offsetX = nullptr;
    QDoubleSpinBox* m_offsetY = nullptr;
    QDoubleSpinBox* m_offsetZ = nullptr;
    QDoubleSpinBox* m_minDistance = nullptr;
    QDoubleSpinBox* m_maxDistance = nullptr;
    QSlider* m_tintR = nullptr;
    QSlider* m_tintG = nullptr;
    QSlider* m_tintB = nullptr;
    QSlider* m_brightness = nullptr;
    QSlider* m_contrast = nullptr;
    QCheckBox* m_edgeLines = nullptr;

    View3D::LayerId m_current;
    bool m_loading = false;
    bool m_listening = false;
};

} // namespace View3DPlugins
