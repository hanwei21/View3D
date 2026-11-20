#include "View3DPluginEntry.h"

#include "LayerPropertiesDock.h"
#include "SceneLayerService.h"
#include "SceneTreeCoordinator.h"
#include "SceneTreeDock.h"
#include "ViewExtension.h"

#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IPluginContainer.h>
#include <IDataOpen.h>
#include <IRenderScene.h>
#include <ISceneTreeDock.h>
#include <ISceneTreeMenu.h>
#include <IUiObjectRegistry.h>
#include <IViewportLayout.h>
#include <View3DObjectIds.h>

#include <QWidget>

#include <memory>

void registerViewportContentsViewFactory(RF::PluginEngine::PluginFactoryRegistry*);

namespace {

using namespace RF::PluginEngine;

// 属性面板存在与否取决于配置（dw.properties 可以没被声明），所以接线是可失败的。
void bindPropertiesDock(IPluginContainer& container, View3DPlugins::SceneLayerService& service,
                        View3DPlugins::SceneTreeCoordinator& coordinator)
{
    auto* dock = container.objectAs<RF::PluginEngine::IDockWindow>(
        QStringLiteral("ui.dw.properties"));
    if (auto* properties = dynamic_cast<View3DPlugins::LayerPropertiesDock*>(dock))
        properties->bind(service, coordinator);
}

class SceneWindowCommand final : public ICommand {
  public:
    SceneWindowCommand(QString name, QString caption, QString tooltip, QString dockName)
        : m_name(std::move(name)),
          m_caption(std::move(caption)),
          m_tooltip(std::move(tooltip)),
          m_dockName(std::move(dockName))
    {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("window"); }
    bool getChecked() const override
    {
        QWidget* widget = dockWidget();
        return widget && widget->isVisible();
    }
    bool getEnabled() const override { return dockWidget() != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_tooltip; }
    void onClick() override {}
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    QWidget* dockWidget() const
    {
        auto* ui = m_application ? m_application->getUiObjectRegistry() : nullptr;
        return ui ? ui->widget(m_dockName) : nullptr;
    }
    IApplication* m_application = nullptr;
    QString m_name;
    QString m_caption;
    QString m_tooltip;
    QString m_dockName;
};

class SceneExtension final : public IExtension {
  public:
    bool onCreate(IApplication* application) override
    {
        m_application = application;
        auto* container = application ? application->getPluginContainer() : nullptr;
        auto* scene = container
            ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
            : nullptr;
        if (!scene)
            return false;

        m_service = std::make_unique<View3DPlugins::SceneLayerService>(*scene);
        container->publishAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers,
                                                          m_service.get(), this);

        // 注册表由宿主发布，先于所有插件创建；这里只消费，不拥有。
        m_menuRegistry = container->objectAs<View3D::ISceneTreeMenuRegistry>(
            View3D::ServiceIds::SceneTreeMenu);
        m_dataOpenRegistry = container->objectAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen);
        auto* layout = container->objectAs<View3D::IViewportLayout>(
            View3D::ServiceIds::ViewportLayout);
        auto* dock = container->objectAs<View3D::ISceneTreeDock>(View3D::ServiceIds::SceneTreeDock);
        if (dock && dock->sceneTree() && dock->sceneRootItem()) {
            m_coordinator = std::make_unique<View3DPlugins::SceneTreeCoordinator>(
                *m_service, *dock->sceneTree(), *dock->sceneRootItem(),
                dock->sceneSearchEdit(), dock->sceneTypeFilter(), layout,
                m_menuRegistry, m_dataOpenRegistry);
            m_service->addChangeCallback([this](const View3DPlugins::LayerChange& change) {
                if (m_coordinator)
                    m_coordinator->handleChange(change);
            });
            m_coordinator->refresh();

            // 属性面板在配置构建阶段就已创建（dock 早于扩展），这里把服务与选中
            // 通知接上去；面板本身不查服务，也不认识任何具体图层类型。
            bindPropertiesDock(*container, *m_service, *m_coordinator);
        }
        return true;
    }

    void onDestroy() override
    {
        // 回调捕获的是 this，服务与本扩展同生共死，销毁顺序由下面的 reset 保证。
        m_coordinator.reset();
        if (m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                if (m_service)
                    container->unpublishObject(View3D::ServiceIds::SceneLayers,
                                               m_service.get());
            }
        }
        m_menuRegistry = nullptr;
        m_dataOpenRegistry = nullptr;
        m_service.reset();
        m_application = nullptr;
    }

  private:
    IApplication* m_application = nullptr;
    std::unique_ptr<View3DPlugins::SceneLayerService> m_service;
    View3D::ISceneTreeMenuRegistry* m_menuRegistry = nullptr;
    View3D::IDataOpenRegistry* m_dataOpenRegistry = nullptr;
    std::unique_ptr<View3DPlugins::SceneTreeCoordinator> m_coordinator;
};

IPlugin* createSceneCommand(const PluginCreationContext& context)
{
    const QString name = context.attributes.value(QStringLiteral("name"));
    if (name == QStringLiteral("cmd.window.properties")) {
        return new SceneWindowCommand(name, QStringLiteral("图层属性"),
                                      QStringLiteral("显示或隐藏图层属性面板"),
                                      QStringLiteral("dw.properties"));
    }
    return new SceneWindowCommand(QStringLiteral("cmd.window.scene"), QStringLiteral("场景树"),
                                  QStringLiteral("显示或隐藏场景树"), QStringLiteral("dw.scene"));
}

void registerSceneFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.sceneview"), QStringLiteral("factory.create_dockwindow"),
        QStringLiteral("dockwindow"),
        [](const PluginCreationContext& context) -> IPlugin* {
            const QString name = context.attributes.value(QStringLiteral("name"));
            const QString text = context.attributes.value(QStringLiteral("text"));
            if (name == QStringLiteral("dw.properties"))
                return new View3DPlugins::LayerPropertiesDock(name, text);
            return new View3DPlugins::SceneTreeDock(name, text);
        });
    for (const QString& type : {QStringLiteral("command"), QStringLiteral("checkcommand")}) {
        registry->registerCreator(QStringLiteral("plugin.view3d.sceneview"),
                                  QStringLiteral("factory.create_scene_command"), type,
                                  createSceneCommand);
    }
    registry->registerCreator(
        QStringLiteral("plugin.view3d.sceneview"), QStringLiteral("extension.scene"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new SceneExtension(); });
}

void registerViewExtension(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.sceneview"), QStringLiteral("extension.view"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* {
            return new View3DPlugins::ViewExtension();
        });
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.sceneview",
                           registerSceneFactories(registry);
                           registerViewExtension(registry);
                           registerViewportContentsViewFactory(registry);)
