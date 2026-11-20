#include "View3DPluginEntry.h"

#include "MeasureDock.h"
#include "MeasureSession.h"

#include <IApplication.h>
#include <ICommand.h>
#include <IExtension.h>
#include <IPluginContainer.h>
#include <IUiObjectRegistry.h>

#include <QWidget>

#include <memory>
#include <utility>

namespace {

using namespace RF::PluginEngine;
using View3DPlugins::MeasureTool;

View3DPlugins::IMeasureToolService* measureService(IApplication* application)
{
    auto* container = application ? application->getPluginContainer() : nullptr;
    return container
        ? container->objectAs<View3DPlugins::IMeasureToolService>(
              View3DPlugins::MeasureToolServiceName)
        : nullptr;
}

class MeasureToolCommand final : public ICommand {
  public:
    MeasureToolCommand(QString name, QString caption, MeasureTool tool)
        : m_name(std::move(name)), m_caption(std::move(caption)), m_tool(tool)
    {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("measure"); }
    bool getChecked() const override
    {
        auto* service = measureService(m_application);
        return service && service->measureToolActive(m_tool);
    }
    bool getEnabled() const override { return measureService(m_application) != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return {}; }
    void onClick() override
    {
        if (auto* service = measureService(m_application))
            service->setMeasureTool(m_tool, !getChecked());
    }
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    QString m_name;
    QString m_caption;
    MeasureTool m_tool = MeasureTool::None;
    IApplication* m_application = nullptr;
};

class SnappingCommand final : public ICommand {
  public:
    QString getCaption() const override { return QStringLiteral("测量吸附"); }
    QString getCategory() const override { return QStringLiteral("measure"); }
    bool getChecked() const override
    {
        auto* service = measureService(m_application);
        return service && service->snappingEnabled();
    }
    bool getEnabled() const override { return measureService(m_application) != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.measure.snapping"); }
    QString getTooltip() const override { return {}; }
    void onClick() override
    {
        if (auto* service = measureService(m_application))
            service->setSnappingEnabled(!getChecked());
    }
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    IApplication* m_application = nullptr;
};

class MeasurePanelCommand final : public ICommand {
  public:
    QString getCaption() const override { return QStringLiteral("测量结果"); }
    QString getCategory() const override { return QStringLiteral("measure"); }
    bool getChecked() const override
    {
        auto* ui = m_application ? m_application->getUiObjectRegistry() : nullptr;
        auto* widget = ui ? ui->widget(QStringLiteral("dw.measurements")) : nullptr;
        return widget && widget->isVisible();
    }
    bool getEnabled() const override { return m_application != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.window.measurements"); }
    QString getTooltip() const override { return {}; }
    void onClick() override {}
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    IApplication* m_application = nullptr;
};

class ToolsExtension final : public IExtension {
  public:
    bool onCreate(IApplication* application) override
    {
        if (!application)
            return false;
        m_application = application;
        auto* container = application->getPluginContainer();
        if (!container)
            return false;
        m_session = std::make_unique<View3DPlugins::MeasureSession>(application);
        if (!m_session->start()) {
            m_session.reset();
            return false;
        }
        container->publishAs<View3DPlugins::IMeasureToolService>(
            View3DPlugins::MeasureToolServiceName, m_session.get(), this);
        return true;
    }

    void onDestroy() override
    {
        if (m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                if (m_session) {
                    container->unpublishObject(
                        View3DPlugins::MeasureToolServiceName,
                        static_cast<View3DPlugins::IMeasureToolService*>(m_session.get()));
                }
            }
        }
        if (m_session)
            m_session->stop();
        m_session.reset();
        m_application = nullptr;
    }

  private:
    IApplication* m_application = nullptr;
    std::unique_ptr<View3DPlugins::MeasureSession> m_session;
};

IPlugin* createMeasureCommand(const PluginCreationContext& context)
{
    const QString name = context.attributes.value(QStringLiteral("name"));
    const QString caption = context.attributes.value(QStringLiteral("caption"));
    if (name == QStringLiteral("cmd.measure.coordinate"))
        return new MeasureToolCommand(name, caption, MeasureTool::Coordinate);
    if (name == QStringLiteral("cmd.measure.distance"))
        return new MeasureToolCommand(name, caption, MeasureTool::Distance);
    if (name == QStringLiteral("cmd.measure.polyline"))
        return new MeasureToolCommand(name, caption, MeasureTool::Polyline);
    if (name == QStringLiteral("cmd.measure.angle"))
        return new MeasureToolCommand(name, caption, MeasureTool::Angle);
    if (name == QStringLiteral("cmd.measure.area"))
        return new MeasureToolCommand(name, caption, MeasureTool::Area);
    if (name == QStringLiteral("cmd.measure.snapping"))
        return new SnappingCommand();
    if (name == QStringLiteral("cmd.window.measurements"))
        return new MeasurePanelCommand();
    return nullptr;
}

void registerToolsFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.tools"), QStringLiteral("extension.tools"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new ToolsExtension(); });
    for (const QString& type : {QStringLiteral("command"), QStringLiteral("checkcommand")}) {
        registry->registerCreator(QStringLiteral("plugin.view3d.tools"),
                                  QStringLiteral("factory.create_command"), type,
                                  createMeasureCommand);
    }
    registry->registerCreator(
        QStringLiteral("plugin.view3d.tools"), QStringLiteral("factory.create_dockwindow"),
        QStringLiteral("dockwindow"),
        [](const PluginCreationContext& context) -> IPlugin* {
            return new View3DPlugins::MeasureDock(
                context.attributes.value(QStringLiteral("name")),
                context.attributes.value(QStringLiteral("text")));
        });
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.tools", registerToolsFactories(registry);)
