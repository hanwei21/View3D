#include "PluginUiHost.h"
#include "ThemeController.h"
#include <IChoiceCommand.h>
#include <ICommand.h>
#include <IApplication.h>
#include <IPluginContainer.h>
#include <IContentsView.h>
#include <IDockWindow.h>
#include <IUiObjectRegistry.h>
#include <IServicePublisher.h>
#include <QAction>
#include <QActionGroup>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QMainWindow>
#include <QObject>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <SARibbonBar.h>
#include <SARibbonButtonGroupWidget.h>
#include <SARibbonCategory.h>
#include <SARibbonContextCategory.h>
#include <SARibbonPanel.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <utility>

namespace View3DApplication {
namespace {

QIcon configuredIcon(const QString &icon, const QDir &configDir)
{
    if (icon.isEmpty())
        return {};
    return QIcon(QFileInfo(icon).isAbsolute() ? icon : configDir.filePath(icon));
}

} // namespace


PluginUiHost::PluginUiHost(QMainWindow &w, RF::PluginEngine::IApplication &application,
                           const QSet<QString> &loadedPluginIds)
    : m_window(w), m_application(application), m_loadedPluginIds(loadedPluginIds) {
}

PluginUiHost::~PluginUiHost() {
    auto *container = m_application.getPluginContainer();
    if (container) {
        for (auto *command : container->getCommands())
            if (command)
                command->onDestroy();
        for (auto *dock : container->getDockWindows())
            if (dock)
                dock->onDestroy();
        for (auto *view : container->getContentsViews())
            if (view)
                view->onDestroy();
    }
    for (auto iterator = m_publishedObjects.crbegin(); iterator != m_publishedObjects.crend();
         ++iterator)
        m_application.getPluginContainer()->unpublishObject(iterator->name, iterator->object);
    for (auto iterator = m_dockContents.cbegin(); iterator != m_dockContents.cend(); ++iterator)
        m_application.getPluginContainer()->unpublishObject(QStringLiteral("ui.") + iterator.key(), iterator.value());
    for (auto iterator = m_contentsViews.cbegin(); iterator != m_contentsViews.cend(); ++iterator)
        m_application.getPluginContainer()->unpublishObject(QStringLiteral("ui.") + iterator.key(), iterator.value());
    for (auto iterator = m_actions.cbegin(); iterator != m_actions.cend(); ++iterator)
        m_application.getUiObjectRegistry()->unregisterAction(iterator.key(), iterator.value());
    for (auto iterator = m_dockWidgets.cbegin(); iterator != m_dockWidgets.cend(); ++iterator)
        m_application.getUiObjectRegistry()->unregisterWidget(iterator.key(), iterator.value());
    for (auto iterator = m_contentsViews.cbegin(); iterator != m_contentsViews.cend(); ++iterator)
        if (auto *view = dynamic_cast<RF::PluginEngine::IContentsView *>(iterator.value()))
            m_application.getUiObjectRegistry()->unregisterWidget(iterator.key(), view->getWidget());
}
bool PluginUiHost::buildRibbon(SARibbonBar *ribbon, const QList<View3D::RibbonPageLayout> &pages,
                               const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
                               const QDir &configDir, QStringList *warnings) {
    if (!ribbon)
        return false;
    m_ribbon = ribbon;
    for (const auto &p : pages) {
        if (!p.context.isEmpty())
            continue;
        auto *page = ribbon->addCategoryPage(p.text);
        if (!buildPageContents(page, p, configDir, warnings))
            return false;
    }
    QHash<QString, SARibbonContextCategory *> contexts;
    for (const auto &p : pages) {
        if (p.context.isEmpty())
            continue;
        auto *context = contexts.value(p.context, nullptr);
        if (!context) {
            context = ribbon->addContextCategory(p.context, QColor(), p.context);
            contexts.insert(p.context, context);
        }
        auto *page = ribbon->addCategoryPage(p.text);
        if (!buildPageContents(page, p, configDir, warnings))
            return false;
        context->addCategoryPage(page);
    }
    m_contextCategories = contexts;
    if (!rightButtons.isEmpty() &&
        !buildRightButtons(ribbon->activeRightButtonGroup(), rightButtons, configDir,
                           warnings)) {
        return false;
    }
    return warnings ? warnings->isEmpty() : true;
}

bool PluginUiHost::buildPageContents(SARibbonCategory *page,
                                     const View3D::RibbonPageLayout &p,
                                     const QDir &configDir, QStringList *warnings) {
    if (!page)
        return false;
    for (const auto &g : p.groups) {
        auto *panel = page->addPanel(g.text);
        for (const auto &i : g.items) {
            if (i.beginGroup)
                panel->addSeparator();
            if (i.type.compare(QStringLiteral("dropdown"), Qt::CaseInsensitive) == 0) {
                createDropdown(panel, i, configDir, warnings);
                continue;
            }
            QAction *action = createCommandAction(i, configDir, warnings);
            if (!action)
                continue;
            if (i.iconSize.compare(QStringLiteral("large"), Qt::CaseInsensitive) == 0)
                panel->addLargeAction(action);
            else
                panel->addSmallAction(action);
        }
    }
    return true;
}

RF::PluginEngine::ICommand *PluginUiHost::createCommandObject(
    const View3D::RibbonGroupLayout::Item &i, QStringList *warnings)
{
    auto *container = m_application.getPluginContainer();
    auto *command = container
        ? container->getCommands().value(i.name, nullptr)
        : nullptr;
    if (!command) {
        if (warnings)
            warnings->append(QStringLiteral("命令工厂不可用：%1").arg(i.name));
        return nullptr;
    }
    command->onCreate(&m_application);
    return command;
}

QAction *PluginUiHost::createCommandAction(const View3D::RibbonGroupLayout::Item &i,
                                           const QDir &configDir, QStringList *warnings)
{
    auto *command = createCommandObject(i, warnings);
    if (!command)
        return nullptr;
    auto *action =
        new QAction(i.caption.isEmpty() ? command->getCaption() : i.caption, &m_window);
    action->setObjectName(i.name);
    action->setToolTip(i.hint.isEmpty() ? command->getTooltip() : i.hint);
    action->setCheckable(
        i.type.compare(QStringLiteral("checkcommand"), Qt::CaseInsensitive) == 0);
    action->setChecked(action->isCheckable() && (i.checked || command->getChecked()));
    action->setEnabled(i.enabled && command->getEnabled());
    if (!i.shortcut.isEmpty())
        action->setShortcut(QKeySequence(i.shortcut));
    if (!i.icon.isEmpty())
        action->setIcon(configuredIcon(i.icon, configDir));
    if (!i.exclusiveGroup.isEmpty()) {
        auto *group = m_groups.value(i.exclusiveGroup, nullptr);
        if (!group) {
            group = new QActionGroup(&m_window);
            group->setExclusive(true);
            m_groups.insert(i.exclusiveGroup, group);
        }
        action->setCheckable(true);
        group->addAction(action);
    }
    QObject::connect(action, &QAction::triggered, &m_window, [this, command] {
        command->onClick();
        refreshCommandStates();
    });
    m_actions.insert(i.name, action);
    m_actionPluginIds.insert(i.name, i.pluginId);
    m_actionConfiguredEnabled.insert(i.name, i.enabled);
    m_application.getUiObjectRegistry()->registerAction(i.name, action);
    return action;
}

QAction *PluginUiHost::createDropdown(SARibbonPanel *panel,
                                      const View3D::RibbonGroupLayout::Item &i,
                                      const QDir &configDir, QStringList *warnings)
{
    if (!panel)
        return nullptr;
    auto *command = createCommandObject(i, warnings);
    if (!command)
        return nullptr;
    auto *choice = dynamic_cast<RF::PluginEngine::IChoiceCommand *>(command);
    if (!choice) {
        if (warnings)
            warnings->append(QStringLiteral("下拉命令接口不可用：%1").arg(i.name));
        return nullptr;
    }
    if (i.options.isEmpty() && choice->choiceOptions().isEmpty()) {
        if (warnings)
            warnings->append(QStringLiteral("下拉项缺少 option：%1").arg(i.name));
        return nullptr;
    }
    auto *combo = new QComboBox(&m_window);
    combo->setObjectName(i.name);
    m_choiceOptions.insert(i.name, i.options);
    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &m_window,
                     [this, combo, choice](int index) {
                         if (index >= 0)
                             choice->selectValue(combo->itemData(index).toString());
                         QTimer::singleShot(0, &m_window,
                                            [this] { refreshCommandStates(); });
                     });
    QAction *action = panel->addSmallWidget(combo);
    if (!action) {
        m_choiceOptions.remove(i.name);
        delete combo;
        return nullptr;
    }
    action->setObjectName(i.name);
    action->setText(i.caption.isEmpty() ? choice->getCaption() : i.caption);
    action->setToolTip(i.hint.isEmpty() ? choice->getTooltip() : i.hint);
    action->setEnabled(i.enabled && choice->getEnabled());
    if (!i.icon.isEmpty())
        action->setIcon(configuredIcon(i.icon, configDir));
    m_choices.insert(i.name, combo);
    syncChoiceItems(i.name, choice);
    m_actions.insert(i.name, action);
    m_actionPluginIds.insert(i.name, i.pluginId);
    m_actionConfiguredEnabled.insert(i.name, i.enabled);
    m_application.getUiObjectRegistry()->registerAction(i.name, action);
    return action;
}

void PluginUiHost::syncChoiceItems(const QString &name,
                                   RF::PluginEngine::IChoiceCommand *choice)
{
    auto *combo = m_choices.value(name, nullptr);
    if (!combo || !choice)
        return;
    // XML 静态项打底，命令返回的动态项只补没有的 value：参照系列表这类
    // 「运行期会长出来」的选项就走这条路。配置里声明过的值保留配置文案，
    // 显示名是配置文件定的，不该被运行期同名项覆盖（宿主也按这个校配置）。
    QList<RF::PluginEngine::ChoiceOption> options;
    for (const auto &option : m_choiceOptions.value(name))
        options.append({option.value, option.caption, option.group});
    for (const auto &extra : choice->choiceOptions()) {
        bool known = false;
        for (const auto &existing : options) {
            if (existing.value == extra.value) {
                known = true;
                break;
            }
        }
        if (!known)
            options.append(extra);
    }
    const QString group = choice->currentOptionGroup();
    const QString current = choice->currentValue();
    bool currentVisible = false;
    for (const auto &option : options) {
        if (option.value == current && (group.isEmpty() || option.group == group))
            currentVisible = true;
    }
    if (!currentVisible && !current.isEmpty()) {
        // 当前值不在列表里（例如手打的自定义 EPSG）：补一条，否则下拉显示的和
        // 实际生效的不是一个东西。
        options.append({current, current, group});
    }
    QString signature;
    for (const auto &option : options) {
        if (!group.isEmpty() && option.group != group)
            continue;
        signature += option.value + QLatin1Char('|') + option.caption + QLatin1Char('|');
    }
    if (m_choiceGroups.value(name, QString()) != signature) {
        QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto &option : options) {
            if (!group.isEmpty() && option.group != group)
                continue;
            combo->addItem(option.caption, option.value);
        }
        m_choiceGroups.insert(name, signature);
    }
    const int index = combo->findData(current);
    if (index >= 0 && combo->currentIndex() != index) {
        QSignalBlocker blocker(combo);
        combo->setCurrentIndex(index);
    }
}

bool PluginUiHost::buildRightButtons(SARibbonButtonGroupWidget *group,
                                     const QList<View3D::RibbonGroupLayout::Item> &items,
                                     const QDir &configDir, QStringList *warnings)
{
    if (!group)
        return false;
    for (const auto &i : items) {
        QAction *action = createCommandAction(i, configDir, warnings);
        if (!action)
            continue;
        group->addAction(action);
        auto *button = qobject_cast<QToolButton *>(group->widgetForAction(action));
        if (!button)
            continue;
        if (i.name == QStringLiteral("cmd.ribbon.minimize") && m_ribbon) {
            const auto applyMode = [button, action](bool minimized) {
                action->setChecked(minimized);
                button->setArrowType(minimized ? Qt::DownArrow : Qt::UpArrow);
            };
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            applyMode(m_ribbon->isMinimumMode());
            QObject::connect(m_ribbon, &SARibbonBar::ribbonModeChanged, action,
                             [applyMode](SARibbonBar::RibbonMode mode) {
                                 applyMode(mode == SARibbonBar::MinimumRibbonMode);
                             });
            continue;
        }
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    return true;
}

void PluginUiHost::setContextVisible(const QString &contextId) {
    if (!m_ribbon)
        return;
    for (auto it = m_contextCategories.cbegin(); it != m_contextCategories.cend(); ++it)
        m_ribbon->setContextCategoryVisible(it.value(), it.key() == contextId);
}

bool PluginUiHost::buildContentsViews(ads::CDockManager *manager,
                                      const QList<View3D::ContentsViewLayoutEntry> &entries,
                                      const QDir &configDir, QStringList *warnings) {
    if (!manager)
        return false;
    for (const auto &entry : entries) {
        if (!m_loadedPluginIds.contains(entry.pluginId))
            continue;
        auto *container = m_application.getPluginContainer();
        auto *view = container
            ? container->getContentsViews().value(entry.name, nullptr)
            : nullptr;
        if (!view || !view->getWidget()) {
            if (warnings)
                warnings->append(QStringLiteral("内容视图工厂不可用：%1").arg(entry.name));
            continue;
        }
        view->onCreate(&m_application);
        view->onActive(true);
        m_application.getUiObjectRegistry()->registerWidget(entry.name, view->getWidget());
        m_contentsViews.insert(entry.name, view);
        container->publishAs<RF::PluginEngine::IContentsView>(
            QStringLiteral("ui.") + entry.name, view, view);
        auto *dock = new ads::CDockWidget(entry.text, manager);
        dock->setObjectName(entry.name + QStringLiteral(".dock"));
        if (!entry.icon.isEmpty())
            dock->setIcon(configuredIcon(entry.icon, configDir));
        dock->setWidget(view->getWidget(), ads::CDockWidget::ForceNoScrollArea);
        auto *dockArea = manager->setCentralWidget(dock);
        if (!dockArea) {
            if (warnings)
                warnings->append(QStringLiteral("中心内容视图不可用：%1").arg(entry.name));
            continue;
        }
        dockArea->setAllowedAreas(ads::OuterDockAreas);
        dock->toggleView(true);
        dock->raise();
        m_contentsDockWidgets.insert(entry.name, dock);
        }
    return warnings ? warnings->isEmpty() : true;
}

bool PluginUiHost::buildDockWindows(ads::CDockManager *manager,
                                    const QList<View3D::DockLayoutEntry> &entries,
                                    const QDir &configDir, QStringList *warnings) {
    if (!manager)
        return false;
    const auto areaFor = [](const QString &area) {
        if (area.compare(QStringLiteral("left"), Qt::CaseInsensitive) == 0)
            return ads::LeftDockWidgetArea;
        if (area.compare(QStringLiteral("bottom"), Qt::CaseInsensitive) == 0)
            return ads::BottomDockWidgetArea;
        if (area.compare(QStringLiteral("top"), Qt::CaseInsensitive) == 0)
            return ads::TopDockWidgetArea;
        return ads::RightDockWidgetArea;
    };
    for (const auto &entry : entries) {
        if (entry.pluginId != View3DApplication::hostCommandPluginId() &&
            !m_loadedPluginIds.contains(entry.pluginId))
            continue;
        auto *container = m_application.getPluginContainer();
        auto *dockContent = container
            ? container->getDockWindows().value(entry.name, nullptr)
            : nullptr;
        if (!dockContent || !dockContent->getWidget()) {
            if (warnings)
                warnings->append(QStringLiteral("停靠窗口工厂不可用：%1").arg(entry.name));
            continue;
        }
        dockContent->onCreate(&m_application);
        auto *dock = new ads::CDockWidget(entry.text, manager);
        dock->setObjectName(entry.name);
        if (!entry.icon.isEmpty())
            dock->setIcon(configuredIcon(entry.icon, configDir));
        dock->setWidget(dockContent->getWidget(), dockContent->getScrollable()
                                                    ? ads::CDockWidget::AutoScrollArea
                                                    : ads::CDockWidget::ForceNoScrollArea);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, entry.floatable);
        manager->addDockWidget(areaFor(entry.area), dock);
        if (entry.preferredWidth > 0 || entry.preferredHeight > 0)
            dockContent->getWidget()->resize(qMax(1, entry.preferredWidth),
                                             qMax(1, entry.preferredHeight));
        if (entry.floating)
            dock->setFloating();
        dock->toggleView(entry.visible);
        m_application.getUiObjectRegistry()->registerWidget(entry.name, dock);
        if (auto *action =
                m_actions.value(entry.attributes.value(QStringLiteral("toggleCommand")), nullptr)) {
            action->setCheckable(true);
            action->setChecked(entry.visible);
            // 把「命令」和「指示器」分开，不要再让勾选状态去驱动面板：
            //   * 点按钮 -> triggered -> 按 dock 当前真实可见性翻转 toggleView；
            //   * 面板可见性变化 -> visibilityChanged -> 只是同步勾选显示。
            // 之前用 action.toggled -> toggleView，指望反向同步把勾选改回来，但 ADS 的
            // toggleViewAction 既不是 checkable、checked 也从不跟随面板（实测关掉面板后
            // 它一直是 0），toggled/changed 都收不到，于是勾选停在 true 上，用户再点只是
            // 把 true 翻成 false，面板永远回不来。分开之后即使勾选瞬时不准也只是显示问题。
            QObject::connect(action, &QAction::triggered, dock, [dock] {
                dock->toggleView(!dock->isVisible());
            });
            QObject::connect(dock, &ads::CDockWidget::visibilityChanged, action,
                             &QAction::setChecked);
            // 勾选由上面的 visibilityChanged 直接同步，不能让 refreshCommandStates() 再去
            // 轮询命令的 getChecked()：它读 QWidget::isVisible()，在窗口刚 show、布局还没
            // 稳定的那一瞬间可能是 false，于是把面板关掉。
            m_dockToggleCommands.insert(entry.attributes.value(QStringLiteral("toggleCommand")));
        }
        m_dockWidgets.insert(entry.name, dock);
        m_dockContents.insert(entry.name, dockContent);
        container->publishAs<RF::PluginEngine::IDockWindow>(
            QStringLiteral("ui.") + entry.name, dockContent, dockContent);
        if (auto *publisher = dynamic_cast<RF::PluginEngine::IServicePublisher *>(dockContent))
            for (const auto &service : publisher->publishedServices()) {
                if (!service.type) {
                    qWarning("View3D published service without type: %s",
                             service.name.toUtf8().constData());
                    continue;
                }
                container->publishObject(service.name, service.object, *service.type,
                                         dockContent);
                m_publishedObjects.append(service);
            }
        }
    return warnings ? warnings->isEmpty() : true;
}

void PluginUiHost::refreshCommandStates() {
    auto *container = m_application.getPluginContainer();
    if (!container)
        return;
    for (auto *command : container->getCommands()) {
        if (!command)
            continue;
        const QString name = command->getName();
        auto *action = m_actions.value(name, nullptr);
        if (!action)
            continue;
        const bool configuredEnabled = m_actionConfiguredEnabled.value(name, true);
        action->setEnabled(configuredEnabled && command->getEnabled());
        if (action->isCheckable() && !m_dockToggleCommands.contains(name))
            action->setChecked(command->getChecked());
        if (auto *choice = dynamic_cast<RF::PluginEngine::IChoiceCommand *>(command))
            syncChoiceItems(name, choice);
    }
}

void PluginUiHost::exposeUiObjects() {
    // Objects are registered when created so modules can safely dynamic_cast
    // the original IPlugin pointer across multiple-inheritance interfaces.
}
} // namespace View3DApplication
