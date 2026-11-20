#include "View3DPluginEntry.h"

#include "CrsAuthority.h"
#include "CrsLibrary.h"
#include "CrsManagerDock.h"
#include "CrsReference.h"
#include "CrsSelectDialog.h"

#include <IApplication.h>
#include <IChoiceCommand.h>
#include <IExtension.h>
#include <IGeoReference.h>
#include <IMessageCenter.h>
#include <IPluginContainer.h>
#include <IUiObjectRegistry.h>
#include <View3DObjectIds.h>

#include <QCoreApplication>
#include <QDir>
#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QWidget>

#include <iostream>
#include <memory>

namespace {

using namespace RF::PluginEngine;

// 「显示参照系」下拉：常用投影由 ui_config 的 option 列出，其余走自定义输入。
// 这里只切局部轴的取向；逐点重放（矢量/栅格/影像）与瓦片跟随（倾斜摄影/3D Tiles）
// 由各自插件监听 GeoFrameChange 完成。
class DisplayCrsCommand final : public IChoiceCommand {
  public:
    DisplayCrsCommand(QString name, QString caption, QString tooltip)
        : m_name(std::move(name)), m_caption(std::move(caption)), m_tooltip(std::move(tooltip))
    {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("crs"); }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_tooltip; }
    QString getMessage() const override { return {}; }
    bool getChecked() const override { return false; }
    bool getEnabled() const override { return reference() != nullptr; }
    void onClick() override {}   // 下拉只走 selectValue
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

    QString currentValue() const override
    {
        auto* ref = reference();
        if (!ref || !ref->dynamicProjectionEnabled())
            return QStringLiteral("off");   // 默认不启用：局部系就是数据自己的坐标系
        return ref->displayCrs();
    }

    bool selectValue(const QString& value) override
    {
        auto* ref = reference();
        if (!ref)
            return false;
        QString crs = value.trimmed();
        if (crs.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0) {
            if (ref->setDynamicProjectionEnabled(false)) {
                std::cerr << "VIEW3D_CRS_DISPLAY off=1" << std::endl;
                return true;
            }
            return false;
        }
        if (crs.compare(QStringLiteral("choose"), Qt::CaseInsensitive) == 0) {
            auto* container = m_application ? m_application->getPluginContainer() : nullptr;
            auto* library = container
                ? container->objectAs<View3D::ICrsLibrary>(View3D::ServiceIds::CrsLibrary)
                : nullptr;
            auto* browserService = container
                ? container->objectAs<View3D::ICrsBrowser>(View3D::ServiceIds::CrsBrowser)
                : nullptr;
            if (!library || !browserService)
                return false;
            View3DCrs::CrsSelectDialog dialog(library, browserService,
                                              m_application->getMainWindow());
            dialog.preselect(ref->dynamicProjectionEnabled() ? ref->displayCrs() : QString());
            if (dialog.exec() != QDialog::Accepted)
                return false;   // 取消：下拉回到当前值
            crs = dialog.selectedDefinition();
        }
        if (crs.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0) {
            bool ok = false;
            const QString text = QInputDialog::getText(
                m_application ? m_application->getMainWindow() : nullptr,
                QStringLiteral("自定义显示参照系"),
                QStringLiteral("EPSG 代码或 OGC WKT（如 EPSG:4547）"), QLineEdit::Normal,
                ref->displayCrs(), &ok);
            if (!ok)
                return false;   // 取消：下拉回到当前值
            crs = text.trimmed();
        }
        if (crs.isEmpty())
            return false;
        // 选参照系即等于打开动态投影开关（关它要选「不启用」）。
        if (ref->setDisplayCrs(crs)) {
            std::cerr << "VIEW3D_CRS_DISPLAY crs=" << ref->displayCrs().toStdString() << std::endl;
            return true;
        }
        QMessageBox::warning(m_application ? m_application->getMainWindow() : nullptr,
                             QStringLiteral("显示参照系"),
                             QStringLiteral("无法切换到 %1\n%2").arg(crs, ref->lastError()));
        return false;
    }

    // 下拉只放「常用 + 自定义」：内置有六千多条，全塞进 combo 就没法用了，
    // 挑内置要走参照系面板（那边能搜索、能一键加常用）。值都是定义文本
    // （EPSG 码 / proj 串 / WKT），选中后 selectValue 拿它去 setDisplayCrs。
    // 按服务取而不是构造时注入：下拉可能在 crs 扩展之前就随 Ribbon 建好了。
    QList<ChoiceOption> choiceOptions() const override
    {
        QList<ChoiceOption> options;
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        auto* library = container
            ? container->objectAs<View3D::ICrsLibrary>(View3D::ServiceIds::CrsLibrary)
            : nullptr;
        if (!library)
            return options;
        auto* browser = container
            ? container->objectAs<View3D::ICrsBrowser>(View3D::ServiceIds::CrsBrowser)
            : nullptr;
        QStringList seen;
        for (const QString& definition : library->favorites()) {
            if (definition.isEmpty() || seen.contains(definition))
                continue;
            seen.append(definition);
            // 内置库还没枚举过时取不到官方名，就直接用权威码当文案。
            View3D::ICrsBrowser::Item builtin;
            QString caption;
            if (browser && browser->item(definition, &builtin))
                caption = builtin.name;
            else {
                for (const View3D::CrsLibraryEntry& entry : library->entries()) {
                    if (entry.effectiveDefinition() == definition ||
                        entry.definition == definition) {
                        caption = entry.name;
                        break;
                    }
                }
            }
            ChoiceOption option;
            option.value = definition;
            option.caption = QStringLiteral("%1　·　常用")
                                 .arg(caption.isEmpty() ? definition : caption);
            options.append(option);
        }
        for (const View3D::CrsLibraryEntry& entry : library->entries()) {
            const QString value = entry.effectiveDefinition();
            if (seen.contains(value))
                continue;   // 已收藏的那条只列一次，两条同名更乱
            seen.append(value);
            ChoiceOption option;
            option.value = value;
            option.caption = QStringLiteral("%1（%2）　·　自定义")
                                 .arg(entry.name,
                                      entry.code.isEmpty() ? entry.kindLabel() : entry.code);
            options.append(option);
        }
        return options;
    }

  private:
    View3D::IGeoReference* reference() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        return container
            ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
            : nullptr;
    }

    QString m_name;
    QString m_caption;
    QString m_tooltip;
    IApplication* m_application = nullptr;
};

// 「窗口」页的参照系管理勾选：真正的显隐由宿主连到 dock，这里只回读可见状态。
class CrsManagerDockCommand final : public ICommand {
  public:
    QString getCaption() const override { return QStringLiteral("参照系管理"); }
    QString getCategory() const override { return QStringLiteral("window"); }
    bool getChecked() const override
    {
        auto* ui = m_application ? m_application->getUiObjectRegistry() : nullptr;
        auto* widget = ui ? ui->widget(QStringLiteral("dw.crs_manager")) : nullptr;
        return widget && widget->isVisible();
    }
    bool getEnabled() const override { return m_application != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.window.crs_manager"); }
    QString getTooltip() const override { return QStringLiteral("显示或隐藏自定义参照系列表"); }
    void onClick() override {}
    void onCreate(IApplication* application) override { m_application = application; }
    void onDestroy() override { m_application = nullptr; }

  private:
    IApplication* m_application = nullptr;
};

IPlugin* createCrsCommand(const PluginCreationContext& context)
{
    const QString name = context.attributes.value(QStringLiteral("name"));
    if (name == QStringLiteral("cmd.crs.display")) {
        return new DisplayCrsCommand(
            name, context.attributes.value(QStringLiteral("caption")),
            context.attributes.value(QStringLiteral("hint")));
    }
    if (name == QStringLiteral("cmd.window.crs_manager"))
        return new CrsManagerDockCommand();
    return nullptr;
}

// 参照系服务的宿主接线：只发布 IGeoReference，不消费任何服务，
// 所以它在扩展创建轮次里可以最早成功，供影像/导航/导入器/tileset 领用。
class CrsExtension final : public IExtension {
  public:
    bool onCreate(IApplication* application) override
    {
        auto* container = application ? application->getPluginContainer() : nullptr;
        if (!container)
            return false;
        m_reference = std::make_unique<View3DCrs::CrsReference>();
        // PROJ 的椭球/基准数据随包部署在 bin/proj-data；找不到时 PROJ 会用
        // 自身默认查找路径，重投影仍可能对已内置的简单 CRS 生效。
        m_reference->setProjDataDirectory(
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("proj-data")));
        container->publishAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference,
                                                   m_reference.get(), this);
        // 内置参照系（PROJ 权威库）按服务给面板用；第一次 groups() 才真的枚举。
        m_browser = std::make_unique<View3DCrs::CrsAuthority>(m_reference->backend());
        container->publishAs<View3D::ICrsBrowser>(View3D::ServiceIds::CrsBrowser,
                                                 m_browser.get(), this);
        m_library = std::make_unique<View3DCrs::CrsLibrary>(m_reference->backend());
        QString loadNote;
        m_library->load(&loadNote);
        container->publishAs<View3D::ICrsLibrary>(View3D::ServiceIds::CrsLibrary,
                                                 m_library.get(), this);
        // 库一变，下拉的选项与面板的列表都要重建；宿主只在收到这个广播时才回读命令状态。
        m_library->setChangeCallback([application] {
            auto* messages = application ? application->getMessageCenter() : nullptr;
            if (messages)
                messages->publish(View3D::MessageTopics::RefreshCommandStates);
        });
        // 面板可能已经先建好了（宿主建 Dock 的时机不保证在扩展之前），广播一次让它补绑定。
        if (auto* messages = application->getMessageCenter())
            messages->publish(View3D::MessageTopics::CrsLibraryReady);
        m_container = container;
        std::cerr << "VIEW3D_CRS_READY proj=" << (m_reference->backend().available() ? 1 : 0)
                  << " library=" << m_library->entries().size()
                  << " favourites=" << m_library->favorites().size() << std::endl;
        if (!loadNote.isEmpty())
            std::cerr << "VIEW3D_CRS_LIBRARY_NOTE " << loadNote.toStdString() << std::endl;
        return true;
    }

    void onDestroy() override
    {
        if (m_library)
            m_library->setChangeCallback(nullptr);
        if (m_container && m_library) {
            m_container->unpublishObject(View3D::ServiceIds::CrsLibrary,
                                         static_cast<View3D::ICrsLibrary*>(m_library.get()));
        }
        if (m_container && m_browser) {
            m_container->unpublishObject(View3D::ServiceIds::CrsBrowser,
                                         static_cast<View3D::ICrsBrowser*>(m_browser.get()));
        }
        if (m_container && m_reference) {
            m_container->unpublishObject(
                View3D::ServiceIds::GeoReference,
                static_cast<View3D::IGeoReference*>(m_reference.get()));
        }
        // 面板与下拉都是按服务取的，服务先撤，它们自然回到未绑定状态。
        m_library.reset();
        m_browser.reset();
        m_reference.reset();
        m_container = nullptr;
    }

  private:
    std::unique_ptr<View3DCrs::CrsReference> m_reference;
    std::unique_ptr<View3DCrs::CrsLibrary> m_library;
    std::unique_ptr<View3DCrs::CrsAuthority> m_browser;
    IPluginContainer* m_container = nullptr;
};

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.crs", {
    registry->registerCreator(
        QStringLiteral("plugin.view3d.crs"), QStringLiteral("extension.crs"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new CrsExtension(); });
    // Ribbon 的「显示参照系」下拉与「参照系管理」勾选：命令工厂按类型各注册一遍，
    // 宿主建 Ribbon 时找不到工厂项会直接判启动失败，所以 checkcommand 不能漏。
    for (const QString& type :
         {QStringLiteral("command"), QStringLiteral("checkcommand"), QStringLiteral("dropdown")}) {
        registry->registerCreator(QStringLiteral("plugin.view3d.crs"),
                                  QStringLiteral("factory.create_command"), type,
                                  createCrsCommand);
    }
    registry->registerCreator(
        QStringLiteral("plugin.view3d.crs"), QStringLiteral("factory.create_dockwindow"),
        QStringLiteral("dockwindow"),
        [](const PluginCreationContext& context) -> IPlugin* {
            return new View3DCrs::CrsManagerDock(
                context.attributes.value(QStringLiteral("name")),
                context.attributes.value(QStringLiteral("text")));
        });
})
