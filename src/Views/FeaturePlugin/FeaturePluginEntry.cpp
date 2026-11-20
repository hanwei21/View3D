#include "View3DPluginEntry.h"

#include "FeatureTableDock.h"

#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IPluginContainer.h>
#include <IRenderScene.h>
#include <ISceneLayerService.h>
#include <IUiObjectRegistry.h>
#include <IViewportLayout.h>
#include <ModelMesh.h>
#include <View3DObjectIds.h>

#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

namespace {

using namespace RF::PluginEngine;

// 属性表显隐切换（对应 ui_config 的 toggleCommand，实际显隐由宿主连到 dock）。
class FeatureDockCommand final : public ICommand {
  public:
    QString getCaption() const override { return QStringLiteral("属性表"); }
    QString getCategory() const override { return QStringLiteral("window"); }
    bool getChecked() const override
    {
        auto* ui = m_application ? m_application->getUiObjectRegistry() : nullptr;
        auto* widget = ui ? ui->widget(QStringLiteral("dw.feature_table")) : nullptr;
        return widget != nullptr && widget->isVisible();
    }
    bool getEnabled() const override { return m_application != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.window.feature_table"); }
    QString getTooltip() const override { return {}; }
    void onClick() override {}
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    IApplication* m_application = nullptr;
};

// 要素属性表宿主接线：拾取按监听者广播，属性一律取自网格要素表，
// 因此二维矢量、模型、3D Tiles/BIM 共用同一条通路，不依赖地理插件。
class FeatureExtension final : public IExtension {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        m_application = application;
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        auto* scene =
            container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene);
        if (!scene)
            return false;
        m_scene = scene;
        syncPickListeners();
        return true;
    }

    void onDestroy() override
    {
        clearPickListeners();
        if (m_scene)
            m_scene->clearFeatureHighlights();
        if (auto* dock = findDock())
            dock->clearSelection();
        m_scene = nullptr;
        m_application = nullptr;
    }

    // 每个视口各登记一条监听；布局切换新增的视口在下一次拾取时补上。
    void syncPickListeners()
    {
        auto* layout = liveLayout();
        if (!layout)
            return;
        const auto liveViews = layout->views();
        for (auto it = m_pickListeners.begin(); it != m_pickListeners.end();) {
            if (liveViews.contains(it->first))
                ++it;
            else
                it = m_pickListeners.erase(it);
        }
        for (View3D::IRenderSurface* view : liveViews) {
            if (!view || containsView(view))
                continue;
            const View3D::PickListenerId id = view->addPickListener(
                [this](const View3D::ViewportPickResult& result) { handlePick(result); });
            if (id != 0)
                m_pickListeners.emplace_back(view, id);
        }
    }

  private:
    View3D::IViewportLayout* liveLayout() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        return container ? container->objectAs<View3D::IViewportLayout>(
                               View3D::ServiceIds::ViewportLayout)
                         : nullptr;
    }

    bool containsView(View3D::IRenderSurface* view) const
    {
        for (const auto& entry : m_pickListeners) {
            if (entry.first == view)
                return true;
        }
        return false;
    }

    void clearPickListeners()
    {
        auto* layout = liveLayout();
        if (layout) {
            const auto liveViews = layout->views();
            for (const auto& entry : m_pickListeners) {
                if (entry.first && liveViews.contains(entry.first))
                    entry.first->removePickListener(entry.second);
            }
        }
        m_pickListeners.clear();
    }

    View3DFeature::FeatureTableDock* findDock() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        if (!container)
            return nullptr;
        auto* dock = container->getDockWindows().value(QStringLiteral("dw.feature_table"),
                                                       nullptr);
        return dynamic_cast<View3DFeature::FeatureTableDock*>(dock);
    }

    void handlePick(const View3D::ViewportPickResult& result)
    {
        // 新创建的视口没有监听者：任何一次拾取都顺带把它们补齐。
        syncPickListeners();
        if (!m_scene)
            return;
        // highlightModelFeature 内部先清旧高亮，所以每次点选都是替换。
        if (result.hasFeature)
            m_scene->highlightModelFeature(result.renderKey, result.featureTableIndex,
                                           result.featureId);
        else
            m_scene->clearFeatureHighlights();

        View3DFeature::FeatureTableDock* dock = findDock();
        if (!dock)
            return;
        if (!result.hasFeature) {
            dock->clearSelection();
            return;
        }

        View3DFeature::FeatureSelection selection;
        selection.sourceKey = result.renderKey;
        selection.featureId = result.featureId;
        selection.attributes = result.featureProperties;

        // 属性以网格要素表为准：渲染端已按表填好拾取结果，这里只在缺失时兜底，
        // 同时取本表要素总数用于标题（瓦片数据只覆盖当前驻留瓦片）。
        const auto mesh = m_scene->modelLayerMesh(result.renderKey);
        if (mesh && result.featureTableIndex >= 0 &&
            static_cast<std::size_t>(result.featureTableIndex) <
                mesh->featureTables.size()) {
            const View3D::ModelFeatureTableDto& table =
                mesh->featureTables[static_cast<std::size_t>(result.featureTableIndex)];
            selection.tableFeatureCount = table.featureCount;
            selection.tableFeatureCountValid = true;
            if (selection.attributes.isEmpty() && result.featureId >= 0)
                selection.attributes =
                    mesh->featureProperties(result.featureTableIndex, result.featureId);
        }

        if (auto* container = m_application ? m_application->getPluginContainer() : nullptr) {
            auto* layers =
                container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers);
            if (layers)
                selection.layerName =
                    layers->layerName(layers->layerByRenderKey(result.renderKey));
        }
        dock->showFeature(selection);
    }

    IApplication* m_application = nullptr;
    View3D::IRenderScene* m_scene = nullptr;
    std::vector<std::pair<View3D::IRenderSurface*, View3D::PickListenerId>> m_pickListeners;
};

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.features", {
    registry->registerCreator(
        QStringLiteral("plugin.view3d.features"), QStringLiteral("extension.features"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new FeatureExtension(); });
    for (const QString& type : {QStringLiteral("command"), QStringLiteral("checkcommand")}) {
        registry->registerCreator(QStringLiteral("plugin.view3d.features"),
                                  QStringLiteral("factory.create_command"), type,
                                  [](const PluginCreationContext& context) -> IPlugin* {
                                      const QString name =
                                          context.attributes.value(QStringLiteral("name"));
                                      if (name == QStringLiteral("cmd.window.feature_table"))
                                          return new FeatureDockCommand();
                                      return nullptr;
                                  });
    }
    registry->registerCreator(
        QStringLiteral("plugin.view3d.features"), QStringLiteral("factory.create_dockwindow"),
        QStringLiteral("dockwindow"),
        [](const PluginCreationContext& context) -> IPlugin* {
            return new View3DFeature::FeatureTableDock(
                context.attributes.value(QStringLiteral("name")),
                context.attributes.value(QStringLiteral("text")));
        });
})
