#include "HostSmokeChecks.h"

#include "DockLayoutUtils.h"
#include "LogDock.h"
#include "FramelessEdgeFilter.h"

#include <IAppLog.h>
#include <IAppLogManager.h>
#include <IApplication.h>
#include <IPluginContainer.h>
#include <IPluginRegistryService.h>
#include <IRenderService.h>
#include <ISceneTreeDock.h>
#include <IDockWindow.h>
#include <IDataOpen.h>
#include <ICrsBrowser.h>
#include <ICrsLibrary.h>
#include <GlobeMath.h>
#include <IGeoReference.h>
#include <IImageryService.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <ISceneLayerService.h>
#include <ILayerPropertiesDock.h>
#include <ISceneTreeMenu.h>
#include <IOsbLoader.h>
#include <ITilesetLoader.h>
#include <ModelMesh.h>
#include <IUiObjectRegistry.h>
#include <IViewportLayout.h>
#include <PluginServiceIds.h>
#include <View3DObjectIds.h>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QWidget>

#include <SARibbonBar.h>
#include <SARibbonButtonGroupWidget.h>
#include <SARibbonMainWindow.h>

#include "PluginUiHost.h"
#include "ThemeController.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <cstring>
#include <memory>
#include <vector>

namespace View3DApplication {
namespace {

struct ThemePixelSample {
    int ribbonLuma = -1;
    int dockLuma = -1;
};

int averageLuma(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return -1;
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return -1;
    const int stepX = qMax(1, image.width() / 64);
    const int stepY = qMax(1, image.height() / 48);
    qint64 total = 0;
    int samples = 0;
    for (int y = 0; y < image.height(); y += stepY) {
        for (int x = 0; x < image.width(); x += stepX) {
            total += qGray(image.pixel(x, y));
            ++samples;
        }
    }
    return samples > 0 ? static_cast<int>(total / samples) : -1;
}

ThemePixelSample sampleThemePixels(SARibbonMainWindow &window)
{
    ThemePixelSample sample;
    if (auto *ribbon = window.ribbonBar()) {
        ribbon->ensurePolished();
        if (ribbon->width() < 200 || ribbon->height() < 40)
            ribbon->resize(1200, 150);
        sample.ribbonLuma = averageLuma(ribbon->grab());
    }
    if (auto *dock = window.findChild<QWidget *>(QStringLiteral("dw.scene"))) {
        dock->ensurePolished();
        if (dock->width() < 100 || dock->height() < 100)
            dock->resize(320, 480);
        sample.dockLuma = averageLuma(dock->grab());
    }
    return sample;
}

bool verifyThemeSmoke(View3DApplication::ThemeController &theme, SARibbonMainWindow &window,
                      View3DApplication::PluginUiHost &uiHost,
                      const QList<View3D::RibbonPageLayout> &pages,
                      const QString &settingsPath, QString *error)
{
    using Mode = View3DApplication::ThemeController::Mode;
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto triggerMode = [&](Mode mode) {
        auto *action = uiHost.actions().value(mode == Mode::Dark
                                                  ? QStringLiteral("cmd.theme.dark")
                                                  : QStringLiteral("cmd.theme.light"));
        if (!action || !action->isEnabled())
            return false;
        action->trigger();
        return true;
    };
    const auto checkMode = [&](Mode mode) {
        if (!triggerMode(mode))
            return fail(QStringLiteral("主题命令无法触发：%1").arg(theme.modeKey()));
        uiHost.refreshCommandStates();
        QApplication::processEvents();
        if (window.ribbonTheme() != theme.currentRibbonTheme())
            return fail(QStringLiteral("Ribbon 主题未生效：%1").arg(theme.modeKey()));
        if (!theme.ribbonStyleMatches(window.ribbonBar()))
            return fail(QStringLiteral("Ribbon 风格未生效：%1").arg(theme.styleKey()));
        const QPalette expected = View3DApplication::ThemeController::paletteFor(mode);
        if (qApp->palette().color(QPalette::Window) != expected.color(QPalette::Window))
            return fail(QStringLiteral("窗口调色板未生效：%1").arg(theme.modeKey()));
        auto *lightAction = uiHost.actions().value(QStringLiteral("cmd.theme.light"));
        auto *darkAction = uiHost.actions().value(QStringLiteral("cmd.theme.dark"));
        if (!lightAction || !darkAction)
            return fail(QStringLiteral("主题命令项缺失"));
        const bool dark = mode == Mode::Dark;
        if (lightAction->isChecked() == dark || darkAction->isChecked() != dark)
            return fail(QStringLiteral("主题命令勾选状态不一致：%1").arg(theme.modeKey()));
        if (window.styleSheet().size() < 2000)
            return fail(QStringLiteral("Ribbon 主题样式表为空：%1").arg(theme.modeKey()));
        if (dark && !window.styleSheet().contains(QStringLiteral("CDockManager")))
            return fail(QStringLiteral("深色补充样式表缺失"));
        return true;
    };
    const auto report = [&](const ThemePixelSample &pixels) {
        std::cerr << "VIEW3D_THEME_CHECK mode=" << theme.modeKey().toStdString()
                  << " ribbon=" << theme.ribbonThemeKey().toStdString()
                  << " style=" << theme.styleKey().toStdString()
                  << " sheet=" << window.styleSheet().size()
                  << " window="
                  << qApp->palette().color(QPalette::Window).name().toStdString()
                  << " ribbonLuma=" << pixels.ribbonLuma
                  << " dockLuma=" << pixels.dockLuma << '\n';
    };

    const Mode initialMode = theme.mode();
    const QString initialLightKey = theme.lightThemeKey();
    const QString initialDarkKey = theme.darkThemeKey();
    auto *themeCombo = uiHost.choice(QStringLiteral("cmd.theme.variant"));
    if (!themeCombo)
        return fail(QStringLiteral("主题下拉缺失"));

    const View3D::RibbonGroupLayout::Item *themeItem = nullptr;
    for (const View3D::RibbonPageLayout &page : pages) {
        for (const View3D::RibbonGroupLayout &group : page.groups) {
            for (const View3D::RibbonGroupLayout::Item &item : group.items) {
                if (item.name == QStringLiteral("cmd.theme.variant"))
                    themeItem = &item;
            }
        }
    }
    if (!themeItem)
        return fail(QStringLiteral("主题下拉未在配置中声明"));
    const auto groupCount = [themeItem](const QString &group) {
        int count = 0;
        for (const View3D::RibbonGroupLayout::Item::Option &option : themeItem->options)
            if (option.group == group)
                ++count;
        return count;
    };
    const auto groupValue = [themeItem](const QString &group, const QString &current) {
        for (const View3D::RibbonGroupLayout::Item::Option &option : themeItem->options)
            if (option.group == group && option.value != current)
                return option.value;
        return QString();
    };
    const int lightOptionCount = groupCount(QStringLiteral("light"));
    const int darkOptionCount = groupCount(QStringLiteral("dark"));
    if (lightOptionCount == 0 || darkOptionCount == 0)
        return fail(QStringLiteral("主题下拉缺少浅色或深色分组"));

    const auto variantListMatches = [&](Mode mode, int expectedCount,
                                        const QString &current) {
        const int count = themeCombo->count();
        const QString label = mode == Mode::Dark ? QStringLiteral("深色")
                                                 : QStringLiteral("浅色");
        if (count != expectedCount) {
            return fail(QStringLiteral("%1主题选项数量错误：期望 %2，实际 %3")
                            .arg(label)
                            .arg(expectedCount)
                            .arg(count));
        }
        if (themeCombo->currentData().toString() != current)
            return fail(QStringLiteral("主题下拉当前项与%1模式不一致：%2")
                            .arg(label, current));
        return true;
    };

    if (!checkMode(Mode::Light))
        return false;
    if (!variantListMatches(Mode::Light, lightOptionCount, theme.lightThemeKey()))
        return false;
    const QString targetLightKey = groupValue(QStringLiteral("light"), theme.lightThemeKey());
    if (targetLightKey.isEmpty())
        return fail(QStringLiteral("浅色主题缺少可切换的选项"));
    const int lightIndex = themeCombo->findData(targetLightKey);
    if (lightIndex < 0)
        return fail(QStringLiteral("浅色选项未出现在下拉中：%1").arg(targetLightKey));
    themeCombo->setCurrentIndex(lightIndex);
    QApplication::processEvents();
    uiHost.refreshCommandStates();
    if (theme.mode() != Mode::Light || theme.lightThemeKey() != targetLightKey ||
        theme.ribbonThemeKey() != targetLightKey) {
        return fail(QStringLiteral("浅色主题选项未生效：%1").arg(targetLightKey));
    }
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/lightTheme")).toString() != targetLightKey)
            return fail(QStringLiteral("浅色主题选项未持久化：%1").arg(targetLightKey));
    }
    std::cerr << "VIEW3D_THEME_VARIANT_OK mode=light key=" << targetLightKey.toStdString()
              << " options=" << themeCombo->count() << '\n';

    if (!checkMode(Mode::Dark))
        return false;
    if (!variantListMatches(Mode::Dark, darkOptionCount, theme.darkThemeKey()))
        return false;
    const QString targetDarkKey = groupValue(QStringLiteral("dark"), theme.darkThemeKey());
    if (targetDarkKey.isEmpty())
        return fail(QStringLiteral("深色主题缺少可切换的选项"));
    const int darkIndex = themeCombo->findData(targetDarkKey);
    if (darkIndex < 0)
        return fail(QStringLiteral("深色选项未出现在下拉中：%1").arg(targetDarkKey));
    themeCombo->setCurrentIndex(darkIndex);
    QApplication::processEvents();
    uiHost.refreshCommandStates();
    if (theme.mode() != Mode::Dark || theme.darkThemeKey() != targetDarkKey ||
        theme.ribbonThemeKey() != targetDarkKey) {
        return fail(QStringLiteral("深色主题选项未生效：%1").arg(targetDarkKey));
    }
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/darkTheme")).toString() != targetDarkKey)
            return fail(QStringLiteral("深色主题选项未持久化：%1").arg(targetDarkKey));
    }
    std::cerr << "VIEW3D_THEME_VARIANT_OK mode=dark key=" << targetDarkKey.toStdString()
              << " options=" << themeCombo->count() << '\n';

    if (!checkMode(Mode::Light))
        return false;
    if (!variantListMatches(Mode::Light, lightOptionCount, targetLightKey))
        return false;
    const ThemePixelSample lightPixels = sampleThemePixels(window);
    report(lightPixels);
    if (!checkMode(Mode::Dark))
        return false;
    if (!variantListMatches(Mode::Dark, darkOptionCount, targetDarkKey))
        return false;
    const ThemePixelSample darkPixels = sampleThemePixels(window);
    report(darkPixels);
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/theme")).toString() !=
            QStringLiteral("dark"))
            return fail(QStringLiteral("深色主题未写入状态文件"));
    }
    if (!triggerMode(initialMode))
        return fail(QStringLiteral("主题命令无法恢复初始主题"));
    uiHost.refreshCommandStates();
    QApplication::processEvents();
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        const QString expected = initialMode == Mode::Dark ? QStringLiteral("dark")
                                                           : QStringLiteral("light");
        if (persisted.value(QStringLiteral("ui/theme")).toString() != expected)
            return fail(QStringLiteral("初始主题未写回状态文件"));
    }
    if (theme.lightThemeKey() != targetLightKey || theme.darkThemeKey() != targetDarkKey)
        return fail(QStringLiteral("深/浅切换后主题变体丢失：%1/%2")
                        .arg(theme.lightThemeKey(), theme.darkThemeKey()));
    if (!theme.setLightTheme(initialLightKey))
        return fail(QStringLiteral("无法恢复初始浅色主题：%1").arg(initialLightKey));
    if (!theme.setDarkTheme(initialDarkKey))
        return fail(QStringLiteral("无法恢复初始深色主题：%1").arg(initialDarkKey));
    if (!triggerMode(initialMode))
        return fail(QStringLiteral("主题命令无法恢复初始主题模式"));
    uiHost.refreshCommandStates();
    QApplication::processEvents();
    if (!variantListMatches(initialMode,
                            initialMode == Mode::Dark ? darkOptionCount : lightOptionCount,
                            theme.ribbonThemeKey()))
        return false;
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/lightTheme")).toString() != initialLightKey ||
            persisted.value(QStringLiteral("ui/darkTheme")).toString() != initialDarkKey) {
            return fail(QStringLiteral("初始主题变体未写回状态文件"));
        }
    }

    if (lightPixels.ribbonLuma < 0 || darkPixels.ribbonLuma < 0)
        return fail(QStringLiteral("无法渲染主题校验像素"));
    if (lightPixels.ribbonLuma - darkPixels.ribbonLuma < 20)
        return fail(QStringLiteral("Ribbon 明暗差不足：%1/%2")
                        .arg(lightPixels.ribbonLuma)
                        .arg(darkPixels.ribbonLuma));
    if (lightPixels.dockLuma >= 0 && darkPixels.dockLuma >= 0 &&
        lightPixels.dockLuma - darkPixels.dockLuma < 20) {
        return fail(QStringLiteral("停靠面板明暗差不足：%1/%2")
                        .arg(lightPixels.dockLuma)
                        .arg(darkPixels.dockLuma));
    }
    std::cerr << "VIEW3D_THEME_OK lightRibbon=" << lightPixels.ribbonLuma
              << " darkRibbon=" << darkPixels.ribbonLuma
              << " lightDock=" << lightPixels.dockLuma
              << " darkDock=" << darkPixels.dockLuma << '\n';
    return true;
}

bool verifyRibbonLayoutSmoke(View3DApplication::ThemeController &theme,
                             SARibbonMainWindow &window,
                             View3DApplication::PluginUiHost &uiHost,
                             const QString &settingsPath, bool expectMinimizeButton,
                             const QString &minimizeButtonTip, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *ribbon = window.ribbonBar();
    if (!ribbon)
        return fail(QStringLiteral("RibbonBar 不可用"));

    struct StyleExpectation {
        const char *command;
        const char *styleKey;
        SARibbonBar::RibbonStyles style;
    };
    static const StyleExpectation expectations[] = {
        {"cmd.ribbon.style.loose_three", "office", SARibbonBar::RibbonStyleLooseThreeRow},
        {"cmd.ribbon.style.compact_three", "compact-three-row",
         SARibbonBar::RibbonStyleCompactThreeRow},
        {"cmd.ribbon.style.loose_two", "office-two-row", SARibbonBar::RibbonStyleLooseTwoRow},
        {"cmd.ribbon.style.compact_two", "wps", SARibbonBar::RibbonStyleCompactTwoRow},
        {"cmd.ribbon.style.loose_single", "office-single-row",
         SARibbonBar::RibbonStyleLooseSingleRow},
        {"cmd.ribbon.style.compact_single", "compact-single-row",
         SARibbonBar::RibbonStyleCompactSingleRow},
    };

    const QString initialStyle = theme.styleKey();
    for (const StyleExpectation &expectation : expectations) {
        const QString command = QString::fromLatin1(expectation.command);
        const QString styleKey = QString::fromLatin1(expectation.styleKey);
        auto *action = uiHost.actions().value(command);
        if (!action || !action->isEnabled())
            return fail(QStringLiteral("布局命令不可用：%1").arg(command));
        action->trigger();
        uiHost.refreshCommandStates();
        QApplication::processEvents();
        if (ribbon->currentRibbonStyle() != expectation.style)
            return fail(QStringLiteral("Ribbon 布局未生效：%1").arg(styleKey));
        if (!theme.ribbonStyleMatches(ribbon))
            return fail(QStringLiteral("Ribbon 布局查询不一致：%1").arg(styleKey));
        if (!action->isChecked())
            return fail(QStringLiteral("布局命令勾选状态错误：%1").arg(styleKey));
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/ribbonStyle")).toString() != styleKey)
            return fail(QStringLiteral("Ribbon 布局未持久化：%1").arg(styleKey));
        std::cerr << "VIEW3D_RIBBON_STYLE key=" << styleKey.toStdString() << '\n';
    }

    theme.setRibbonStyle(initialStyle);
    uiHost.refreshCommandStates();
    QApplication::processEvents();
    if (!theme.ribbonStyleMatches(ribbon))
        return fail(QStringLiteral("无法恢复初始 Ribbon 布局"));
    {
        QSettings persisted(settingsPath, QSettings::IniFormat);
        if (persisted.value(QStringLiteral("ui/ribbonStyle")).toString() != initialStyle)
            return fail(QStringLiteral("初始 Ribbon 布局未写回状态文件"));
    }

    auto *group = ribbon->rightButtonGroup();
    auto *action = uiHost.actions().value(QStringLiteral("cmd.ribbon.minimize"));
    auto *button = (group && action)
        ? qobject_cast<QToolButton *>(group->widgetForAction(action))
        : nullptr;
    if (!expectMinimizeButton) {
        if (action || button)
            return fail(QStringLiteral("配置关闭后 Ribbon 收起命令仍存在"));
        std::cerr << "VIEW3D_RIBBON_MINIMIZE_OK configured=off\n";
        std::cerr << "VIEW3D_RIBBON_OK styles=6 initial="
                  << initialStyle.toStdString() << '\n';
        return true;
    }
    if (!group)
        return fail(QStringLiteral("右上角按钮组不可用"));
    if (!action || !button)
        return fail(QStringLiteral("右上角 Ribbon 收起命令缺失"));
    if (!minimizeButtonTip.isEmpty() && action->toolTip() != minimizeButtonTip)
        return fail(QStringLiteral("Ribbon 收起命令提示未按配置生效"));
    const bool wasMinimized = ribbon->isMinimumMode();
    if (button->toolButtonStyle() != Qt::ToolButtonIconOnly)
        return fail(QStringLiteral("Ribbon 收起按钮未使用纯图标"));
    if (button->arrowType() != (wasMinimized ? Qt::DownArrow : Qt::UpArrow))
        return fail(QStringLiteral("Ribbon 收起按钮初始箭头方向错误"));
    button->click();
    QApplication::processEvents();
    if (ribbon->isMinimumMode() == wasMinimized)
        return fail(QStringLiteral("Ribbon 收起命令未切换最小模式"));
    if (action->isChecked() != !wasMinimized)
        return fail(QStringLiteral("Ribbon 收起命令勾选状态错误"));
    if (button->arrowType() != (!wasMinimized ? Qt::DownArrow : Qt::UpArrow))
        return fail(QStringLiteral("Ribbon 收起按钮箭头未随状态切换"));
    button->click();
    QApplication::processEvents();
    if (ribbon->isMinimumMode() != wasMinimized)
        return fail(QStringLiteral("Ribbon 收起命令未恢复展开状态"));
    if (action->isChecked() != wasMinimized)
        return fail(QStringLiteral("Ribbon 收起命令未恢复勾选状态"));
    if (button->arrowType() != (wasMinimized ? Qt::DownArrow : Qt::UpArrow))
        return fail(QStringLiteral("Ribbon 收起按钮箭头未恢复初始方向"));

    std::cerr << "VIEW3D_RIBBON_MINIMIZE_OK group=" << group->objectName().toStdString()
              << " arrow=1\n";
    std::cerr << "VIEW3D_RIBBON_OK styles=6 initial=" << initialStyle.toStdString() << '\n';
    return true;
}

bool verifyConfiguredCommandsSmoke(
    View3DApplication::PluginUiHost &uiHost,
    const QList<View3D::RibbonPageLayout> &pages,
    const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
    const QList<View3D::DockLayoutEntry> &docks, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QSet<QString> expectedActions;
    int dropdownCount = 0;
    for (const View3D::RibbonPageLayout &page : pages) {
        for (const View3D::RibbonGroupLayout &group : page.groups) {
            for (const View3D::RibbonGroupLayout::Item &item : group.items) {
                expectedActions.insert(item.name);
                auto *action = uiHost.actions().value(item.name);
                if (!action)
                    return fail(QStringLiteral("配置命令未创建：%1").arg(item.name));
                if (!item.caption.isEmpty() && action->text() != item.caption) {
                    return fail(QStringLiteral("命令文案未按配置生效：%1，期望 %2，实际 %3")
                                    .arg(item.name, item.caption, action->text()));
                }
                if (item.type.compare(QStringLiteral("dropdown"), Qt::CaseInsensitive) != 0)
                    continue;
                ++dropdownCount;
                auto *combo = uiHost.choice(item.name);
                if (!combo)
                    return fail(QStringLiteral("配置下拉控件未创建：%1").arg(item.name));
                bool grouped = false;
                for (const auto &option : item.options)
                    grouped = grouped || !option.group.isEmpty();
                if (!grouped) {
                    // 配置项按序打底，运行期长出来的选项（参照系列表、当前自定义值）
                    // 追加在尾部，所以这里只要求「不少于配置数量」。
                    if (combo->count() < item.options.size()) {
                        return fail(QStringLiteral("下拉选项数量未按配置生效：%1，期望不少于 %2，实际 %3")
                                        .arg(item.name)
                                        .arg(item.options.size())
                                        .arg(combo->count()));
                    }
                    for (int index = 0; index < item.options.size(); ++index) {
                        const auto &option = item.options.at(index);
                        if (combo->itemData(index).toString() != option.value ||
                            combo->itemText(index) != option.caption) {
                            return fail(QStringLiteral("下拉选项未按配置生效：%1[%2]")
                                            .arg(item.name)
                                            .arg(index));
                        }
                    }
                    continue;
                }
                if (combo->count() <= 0)
                    return fail(QStringLiteral("分组下拉没有可见选项：%1").arg(item.name));
                QString activeGroup;
                for (int index = 0; index < combo->count(); ++index) {
                    const QString value = combo->itemData(index).toString();
                    const View3D::RibbonGroupLayout::Item::Option *matched = nullptr;
                    for (const auto &option : item.options) {
                        if (option.value == value) {
                            matched = &option;
                            break;
                        }
                    }
                    if (!matched) {
                        return fail(QStringLiteral("分组下拉选项不在配置中：%1[%2]")
                                        .arg(item.name, value));
                    }
                    if (matched->caption != combo->itemText(index)) {
                        return fail(QStringLiteral("分组下拉文案未按配置生效：%1[%2]")
                                        .arg(item.name, value));
                    }
                    if (matched->group.isEmpty()) {
                        return fail(QStringLiteral("分组下拉包含未分组选项：%1[%2]")
                                        .arg(item.name, value));
                    }
                    if (activeGroup.isEmpty())
                        activeGroup = matched->group;
                    else if (activeGroup != matched->group)
                        return fail(QStringLiteral("分组下拉混用了多个分组：%1").arg(item.name));
                }
                int activeCount = 0;
                for (const auto &option : item.options)
                    if (option.group == activeGroup)
                        ++activeCount;
                if (combo->count() != activeCount) {
                    return fail(QStringLiteral("分组下拉选项数量错误：%1，期望 %2，实际 %3")
                                    .arg(item.name)
                                    .arg(activeCount)
                                    .arg(combo->count()));
                }
            }
        }
    }
    for (const View3D::RibbonGroupLayout::Item &item : rightButtons) {
        expectedActions.insert(item.name);
        auto *action = uiHost.actions().value(item.name);
        if (!action)
            return fail(QStringLiteral("配置命令未创建：%1").arg(item.name));
        if (!item.caption.isEmpty() && action->text() != item.caption) {
            return fail(QStringLiteral("命令文案未按配置生效：%1，期望 %2，实际 %3")
                            .arg(item.name, item.caption, action->text()));
        }
    }
    QSet<QString> actualActions;
    const auto &actions = uiHost.actions();
    for (auto iterator = actions.cbegin(); iterator != actions.cend(); ++iterator)
        actualActions.insert(iterator.key());
    if (actualActions != expectedActions)
        return fail(QStringLiteral("界面动作集合与配置不一致：配置 %1 项，实际 %2 项")
                        .arg(expectedActions.size())
                        .arg(actualActions.size()));
    for (const View3D::DockLayoutEntry &entry : docks) {
        auto *dock = uiHost.dockWidget(entry.name);
        if (!dock)
            return fail(QStringLiteral("配置停靠窗口未创建：%1").arg(entry.name));
        if (dock->windowTitle() != entry.text) {
            return fail(QStringLiteral("停靠窗口标题未按配置生效：%1，期望 %2，实际 %3")
                            .arg(entry.name, entry.text, dock->windowTitle()));
        }
    }
    std::cerr << "VIEW3D_CONFIG_UI_OK actions=" << expectedActions.size()
              << " docks=" << docks.size() << " dropdowns=" << dropdownCount << '\n';
    return true;
}

bool verifyIconsSmoke(View3DApplication::PluginUiHost &uiHost,
                      const QList<View3D::RibbonPageLayout> &activePages,
                      const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
                      const QList<View3D::DockLayoutEntry> &activeDocks,
                      const QList<View3D::ContentsViewLayoutEntry> &activeContentsViews,
                      const QString &configPath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const View3D::LayoutConfig fullLayout =
        View3D::LayoutConfig::load(configPath, nullptr);
    const QDir configDir = QFileInfo(configPath).absoluteDir();

    struct IconUse {
        QString icon;
        QString owner;
    };
    QList<IconUse> declared;
    const auto addDeclared = [&declared](const QString &icon, const QString &owner) {
        if (!icon.isEmpty())
            declared.append({icon, owner});
    };
    for (const View3D::RibbonPageLayout &page : fullLayout.ribbonPages)
        for (const View3D::RibbonGroupLayout &group : page.groups)
            for (const View3D::RibbonGroupLayout::Item &item : group.items)
                addDeclared(item.icon, QStringLiteral("Ribbon/") + item.name);
    for (const View3D::RibbonGroupLayout::Item &item : fullLayout.ribbonRightButtons)
        addDeclared(item.icon, QStringLiteral("Ribbon/") + item.name);
    for (const View3D::DockLayoutEntry &entry : fullLayout.docks)
        addDeclared(entry.icon, QStringLiteral("Dock/") + entry.name);
    for (const View3D::ContentsViewLayoutEntry &entry : fullLayout.contentsViews)
        addDeclared(entry.icon, QStringLiteral("View/") + entry.name);

    // 运行中的界面必须真的把图标挂到了 QAction / CDockWidget 上；只校验文件
    // 存在无法发现配置路径写错或页签 setIcon 被跳过的情况。
    int activeIcons = 0;
    for (const View3D::RibbonPageLayout &page : activePages) {
        for (const View3D::RibbonGroupLayout &group : page.groups) {
            for (const View3D::RibbonGroupLayout::Item &item : group.items) {
                if (item.icon.isEmpty())
                    continue;
                auto *action = uiHost.actions().value(item.name);
                if (!action)
                    return fail(QStringLiteral("图标校验找不到命令：%1").arg(item.name));
                if (action->icon().isNull())
                    return fail(QStringLiteral("Ribbon 命令未挂图标：%1").arg(item.name));
                ++activeIcons;
            }
        }
    }
    for (const View3D::RibbonGroupLayout::Item &item : rightButtons) {
        if (item.icon.isEmpty())
            continue;
        auto *action = uiHost.actions().value(item.name);
        if (!action || action->icon().isNull())
            return fail(QStringLiteral("右上角命令未挂图标：%1").arg(item.name));
        ++activeIcons;
    }
    for (const View3D::DockLayoutEntry &entry : activeDocks) {
        if (entry.icon.isEmpty())
            continue;
        auto *dock = uiHost.dockWidget(entry.name);
        if (!dock || dock->icon().isNull())
            return fail(QStringLiteral("停靠页签未挂图标：%1").arg(entry.name));
        ++activeIcons;
    }
    for (const View3D::ContentsViewLayoutEntry &entry : activeContentsViews) {
        if (entry.icon.isEmpty())
            continue;
        auto *dock = uiHost.contentsDockWidget(entry.name);
        if (!dock || dock->icon().isNull())
            return fail(QStringLiteral("内容视图页签未挂图标：%1").arg(entry.name));
        ++activeIcons;
    }

    const QList<QPair<QColor, const char *>> backgrounds = {
        {QColor(0xf3, 0xf3, 0xf3), "light"},
        {QColor(0x19, 0x24, 0x30), "dark"},
    };
    QSet<QString> checked;
    const auto validateIcon = [&](const QString &iconRef, const QString &owner) {
        const QString path = QFileInfo(iconRef).isAbsolute()
            ? iconRef
            : configDir.filePath(iconRef);
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("%1 的图标文件不存在：%2").arg(owner, path));
        if (checked.contains(path))
            return true;
        checked.insert(path);

        const QIcon icon(path);
        if (icon.isNull())
            return fail(QStringLiteral("%1 的图标无法解码：%2").arg(owner, path));
        const QPixmap pixmap = icon.pixmap(QSize(16, 16), 1.0);
        const QImage image = pixmap.isNull()
            ? QImage()
            : pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
        if (image.isNull() || image.width() < 8 || image.height() < 8)
            return fail(QStringLiteral("%1 的图标无法渲染为 16px：%2").arg(owner, path));
        for (const auto &background : backgrounds) {
            const QColor &base = background.first;
            int visiblePixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb pixel = image.pixel(x, y);
                    if (qAlpha(pixel) < 48)
                        continue;
                    const int delta = qMax(qAbs(qRed(pixel) - base.red()),
                                           qMax(qAbs(qGreen(pixel) - base.green()),
                                                qAbs(qBlue(pixel) - base.blue())));
                    if (delta > 32)
                        ++visiblePixels;
                }
            }
            if (visiblePixels < 3) {
                return fail(QStringLiteral("%1 的图标在%2背景下不可见：%3")
                                .arg(owner, QString::fromLatin1(background.second), path));
            }
        }
        return true;
    };
    for (const IconUse &use : declared) {
        if (!validateIcon(use.icon, use.owner))
            return false;
    }
    const QDir iconsDir(configDir.filePath(QStringLiteral("resources/icons")));
    const QStringList iconFiles =
        iconsDir.entryList({QStringLiteral("*.svg")}, QDir::Files);
    for (const QString &file : iconFiles) {
        if (!validateIcon(QStringLiteral("resources/icons/") + file,
                          QStringLiteral("Icon/") + file)) {
            return false;
        }
    }

    std::cerr << "VIEW3D_ICONS_OK declared=" << declared.size()
              << " unique=" << checked.size() << " active=" << activeIcons << '\n';
    return true;
}

bool verifyPluginEngineAssemblySmoke(RF::PluginEngine::IApplication &application,
                                     QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    if (!container)
        return fail(QStringLiteral("PluginContainer 不可用"));
    if (!application.getRecentFileManager() || !application.getAppLogManager() ||
        !application.getStateManager() || !application.getMessageCenter() ||
        !application.getWorkSpace() || !application.getDynamicMenuManager())
        return fail(QStringLiteral("PluginEngine 管理服务未初始化"));
    if (!container->objectAs<View3D::IRenderService>(
            View3D::ServiceIds::Renderer))
        return fail(QStringLiteral("Renderer 服务不可用"));
    // 类型安全的注册表：用另一个接口解析同一个 id 必须拿到空指针。
    if (!container->objectType(View3D::ServiceIds::Renderer))
        return fail(QStringLiteral("服务注册表未登记接口类型"));
    if (container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Renderer))
        return fail(QStringLiteral("服务注册表未按接口类型拒绝错误解析"));
    const int commands = container->getCommands().size();
    const int docks = container->getDockWindows().size();
    const int contentsViews = container->getContentsViews().size();
    if (commands == 0)
        return fail(QStringLiteral("PluginEngine 命令容器为空"));
    if (docks == 0)
        return fail(QStringLiteral("PluginEngine 停靠窗口容器为空"));
    std::cerr << "VIEW3D_PLUGIN_ENGINE_OK commands=" << commands
              << " docks=" << docks << " contentsviews=" << contentsViews << '\n';
    return true;
}

bool verifyPluginManagerSmoke(View3DApplication::PluginUiHost &uiHost,
                              RF::PluginEngine::IApplication &application,
                              QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *dock = uiHost.dockWidget(QStringLiteral("dw.plugins"));
    if (!dock) {
        std::cerr << "VIEW3D_PLUGIN_MANAGER_SKIPPED configured=off\n";
        return true;
    }
    auto *widget = dock->widget();
    auto *table = widget
        ? widget->findChild<QTableWidget *>(QStringLiteral("dw.plugins.table"))
        : nullptr;
    auto *summary = widget
        ? widget->findChild<QLabel *>(QStringLiteral("dw.plugins.summary"))
        : nullptr;
    if (!table || !summary)
        return fail(QStringLiteral("插件管理面板控件缺失"));
    auto *container = application.getPluginContainer();
    auto *registry = container
        ? container->objectAs<RF::PluginEngine::IPluginRegistryService>(
              RF::PluginEngine::PluginRegistryServiceId)
        : nullptr;
    if (!registry)
        return fail(QStringLiteral("插件注册表服务不可用"));
    const QList<RF::PluginEngine::PluginPackageStatus> packages = registry->packages();
    if (table->rowCount() != packages.size()) {
        return fail(QStringLiteral("插件管理行数与注册表不一致：期望 %1，实际 %2")
                        .arg(packages.size())
                        .arg(table->rowCount()));
    }
    int loaded = 0;
    for (int row = 0; row < packages.size(); ++row) {
        const auto &status = packages.at(row);
        if (status.loaded)
            ++loaded;
        auto *idItem = table->item(row, 0);
        if (!idItem || idItem->text() != status.id)
            return fail(QStringLiteral("插件管理第 %1 行 ID 不一致").arg(row));
    }
    const QString expectedSummary =
        QStringLiteral("已加载 %1 / 共 %2 个插件").arg(loaded).arg(packages.size());
    if (summary->text() != expectedSummary) {
        return fail(QStringLiteral("插件管理摘要错误：%1").arg(summary->text()));
    }
    auto *command = uiHost.actions().value(QStringLiteral("cmd.window.plugins"));
    if (!command || !command->isCheckable())
        return fail(QStringLiteral("插件管理命令缺失或不可勾选"));
    std::cerr << "VIEW3D_PLUGIN_MANAGER_OK packages=" << packages.size()
              << " loaded=" << loaded << '\n';
    return true;
}

// 手写一张最小的单波段 GeoTIFF（8x8 Int16 高程，带 ModelPixelScale / ModelTiepoint /
// GeoKeyDirectory），用来验证「打开数据」的栅格/地形入口不依赖任何外部数据集。
// GeoKey 必须是 GTModelType=1(Geographic)+GeographicTypeGeoKey=4326：写成 2(投影系)
// 又不给 ProjCSGeoKey 时 GDAL 直接放弃 SRS，栅格就没有源参照系可重投影。
// 像素地面分辨率取 0.0001°（约 11 m/px），整幅约 88 m，切平面曲率可以忽略。
QByteArray buildSmokeGeoTiff()
{
    constexpr quint32 kSize = 8;
    const quint32 pixelsOffset = 8;
    const quint32 pixelsBytes = kSize * kSize * 2;   // Int16 单波段
    const quint32 geoKeysOffset = pixelsOffset + pixelsBytes;   // 136
    const quint32 scaleOffset = geoKeysOffset + 32;             // 168
    const quint32 tiePointOffset = scaleOffset + 24;            // 192
    const quint32 ifdOffset = tiePointOffset + 48;              // 240

    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("II", 2);
    out << quint16(42) << ifdOffset;

    for (quint32 row = 0; row < kSize; ++row)
        for (quint32 col = 0; col < kSize; ++col)
            out << quint16(row * 10);   // 高程从 0 递增到 70

    const quint16 geoKeys[] = {1, 1, 0, 3,
                               1024, 0, 1, 1,   // GTModelTypeGeoKey = Geographic
                               1025, 0, 1, 1,   // GTRasterTypeGeoKey = PixelIsArea
                               2048, 0, 1, 4326 // GeographicTypeGeoKey = EPSG:4326
                              };
    for (const quint16 value : geoKeys)
        out << value;

    out << double(0.0001) << double(0.0001) << double(1.0);    // ModelPixelScale
    out << double(0.0) << double(0.0) << double(0.0)          // 像素原点
        << double(100.0) << double(40.0) << double(0.0);      // 对应的地理坐标

    struct Entry {
        quint16 tag;
        quint16 type;
        quint32 count;
        quint32 value;
    };
    const Entry entries[] = {
        {256, 3, 1, kSize},          // ImageWidth
        {257, 3, 1, kSize},          // ImageLength
        {258, 3, 1, 16},             // BitsPerSample
        {259, 3, 1, 1},              // Compression = none
        {262, 3, 1, 1},              // PhotometricInterpretation = BlackIsZero
        {273, 4, 1, pixelsOffset},    // StripOffsets
        {277, 3, 1, 1},              // SamplesPerPixel
        {278, 3, 1, kSize},          // RowsPerStrip
        {279, 4, 1, pixelsBytes},     // StripByteCounts
        {33550, 12, 3, scaleOffset},     // ModelPixelScale
        {33922, 12, 6, tiePointOffset}, // ModelTiepoint
        {34264, 3, 16, geoKeysOffset},   // GeoKeyDirectory
    };
    out << quint16(sizeof(entries) / sizeof(entries[0]));
    for (const Entry &entry : entries)
        out << entry.tag << entry.type << entry.count << entry.value;
    out << quint32(0);
    return data;
}

std::shared_ptr<View3D::ModelMeshDto> makeSmokeTriangleMesh()
{
    auto mesh = std::make_shared<View3D::ModelMeshDto>();
    View3D::MeshVertexDto a;
    a.x = 0.0; a.y = 0.0; a.z = 0.0;
    View3D::MeshVertexDto b;
    b.x = 1.0; b.y = 0.0; b.z = 0.0;
    View3D::MeshVertexDto c;
    c.x = 0.0; c.y = 1.0; c.z = 0.0;
    mesh->vertices = {a, b, c};
    mesh->indices = {0, 1, 2};
    View3D::ModelPrimitiveDto primitive;
    primitive.firstVertex = 0;
    primitive.vertexCount = 3;
    primitive.firstIndex = 0;
    primitive.indexCount = 3;
    primitive.baseColorR = 0.8;
    primitive.baseColorG = 0.8;
    primitive.baseColorB = 0.8;
    primitive.baseColorA = 1.0;
    primitive.baseColorTextureIndex = 0;
    mesh->primitives.push_back(primitive);
    View3D::ModelTextureDto texture;
    texture.width = 1;
    texture.height = 1;
    texture.rgba8 = {std::byte { 255 }, std::byte { 0 }, std::byte { 0 },
                     std::byte { 255 }};
    mesh->textures.push_back(texture);
    View3D::ModelNodeDto node;
    node.meshIndex = 0;
    node.firstPrimitive = 0;
    node.primitiveCount = 1;
    mesh->nodes.push_back(node);
    mesh->rootNodes.push_back(0);
    return mesh;
}

bool verifySceneContentSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *service = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    if (!service || !scene) {
        std::cerr << "VIEW3D_SCENE_CONTENT_SKIPPED framework_only=1" << std::endl;
        return true;
    }

    const auto mesh = makeSmokeTriangleMesh();
    // 其他插件（如倾斜摄影）可能已经占用场景资源，断言一律相对基线。
    const std::uint64_t baselineVertices = scene->registeredVertexCount();
    const std::uint64_t baselineTextures = scene->registeredTextureCount();

    const View3D::LayerId groupId =
        service->addGroup(QStringLiteral("冒烟测试组"));
    const View3D::LayerId layerId =
        service->addModelLayer(QStringLiteral("冒烟测试图层"),
                               QStringLiteral("smoke:triangle"), mesh, groupId);
    if (groupId.isNull() || layerId.isNull())
        return fail(QStringLiteral("无法登记测试图层"));
    if (!scene->isModelLayerVisible(QStringLiteral("smoke:triangle"))) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层默认不可见"));
    }

    QElapsedTimer timer;
    timer.start();
    const auto waitWhile = [&](const std::function<bool()>& pending) {
        while (timer.elapsed() < 3000 && pending()) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    waitWhile([&] {
        return scene->modelLayerRenderState(QStringLiteral("smoke:triangle")) !=
            View3D::LayerRenderState::Ready;
    });
    if (scene->registeredVertexCount() < baselineVertices + 3) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层未上传到共享场景"));
    }
    if (!service->setLayerVisible(layerId, false) ||
        scene->isModelLayerVisible(QStringLiteral("smoke:triangle"))) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层隐藏失败"));
    }
    if (!service->setLayerVisible(layerId, true) ||
        !scene->isModelLayerVisible(QStringLiteral("smoke:triangle"))) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层恢复可见失败"));
    }
    View3D::RenderBounds bounds;
    if (!scene->modelLayerBounds(QStringLiteral("smoke:triangle"), &bounds) ||
        !bounds.valid) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层缺少包围盒"));
    }
    if (scene->modelLayerRenderState(QStringLiteral("smoke:triangle")) !=
        View3D::LayerRenderState::Ready) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层状态不是 Ready"));
    }
    View3D::ViewportPickResult pick;
    if (!scene->pickRay({0.25, 0.25, 1.0}, {0.0, 0.0, -1.0}, &pick) ||
        pick.renderKey != QStringLiteral("smoke:triangle") || !pick.exactTriangle) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试图层拾取失败"));
    }
    if (scene->registeredTextureCount() != baselineTextures + 1) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("测试纹理未进入场景缓存"));
    }
    const View3D::LayerId secondLayerId =
        service->addModelLayer(QStringLiteral("冒烟测试图层2"),
                               QStringLiteral("smoke:triangle2"), mesh, groupId);
    if (secondLayerId.isNull()) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("无法登记第二个测试图层"));
    }
    timer.restart();
    waitWhile([&] { return scene->registeredVertexCount() < baselineVertices + 6; });
    if (scene->registeredVertexCount() < baselineVertices + 6) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("第二个测试图层未上传"));
    }
    if (scene->registeredTextureCount() != baselineTextures + 1) {
        service->removeLayer(groupId);
        return fail(QStringLiteral("相同纹理未去重"));
    }
    if (!service->removeLayer(groupId)) {
        return fail(QStringLiteral("测试图层移除失败"));
    }
    if (scene->registeredVertexCount() != baselineVertices ||
        scene->registeredTextureCount() != baselineTextures)
        return fail(QStringLiteral("测试图层移除后仍占用场景资源"));
    std::cerr << "VIEW3D_SCENE_CONTENT_OK vertices=6 visible=1 pick=1 textures=1 removed=1" << std::endl;
    return true;
}

bool verifyViewportLayoutSmoke(View3DApplication::PluginUiHost &uiHost,
                               RF::PluginEngine::IApplication &application,
                               QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!layout) {
        std::cerr << "VIEW3D_VIEWPORT_LAYOUT_SKIPPED framework_only=1" << std::endl;
        return true;
    }
    auto *cameraSync = uiHost.actions().value(QStringLiteral("cmd.viewport.camera_sync"));
    if (!cameraSync)
        return fail(QStringLiteral("同步相机命令缺失"));
    if (cameraSync->isChecked())
        return fail(QStringLiteral("同步相机默认应为关闭"));
    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    const auto switchLayout = [&](const QString &command, int expectedCount) {
        auto *action = uiHost.actions().value(command);
        if (!action || !action->isEnabled())
            return fail(QStringLiteral("视口布局命令不可用：%1").arg(command));
        action->trigger();
        QElapsedTimer timer;
        timer.start();
        int lastCount = -1;
        while (timer.elapsed() < 8000) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            const auto viewports = layout->views();
            lastCount = layout->layoutViewportCount();
            bool ready = lastCount == expectedCount;
            for (View3D::IRenderSurface *viewport : viewports)
                ready = ready && viewport->ready();
            if (ready) {
                View3D::IRenderSurface *primary = layout->primaryView();
                const QImage snapshot = primary ? primary->sceneSnapshot() : QImage();
                if (snapshot.isNull() || snapshot.width() <= 0 ||
                    snapshot.height() <= 0) {
                    return fail(QStringLiteral("视口渲染失败：%1").arg(command));
                }
                std::cerr << "VIEW3D_VIEWPORT_LAYOUT_OK command="
                          << command.toStdString() << " count=" << lastCount
                          << " pool=" << viewports.size() << " rendered=1\n";
                return true;
            }
            QThread::msleep(5);
        }
        return fail(QStringLiteral("视口未就绪：%1，期望 %2，实际 %3")
                        .arg(command)
                        .arg(expectedCount)
                        .arg(lastCount));
    };
    if (!switchLayout(QStringLiteral("cmd.viewport.layout_dual"), 2))
        return false;
    {
        const auto viewports = layout->views();
        auto *primary = layout->primaryView();
        auto *secondary = viewports.size() >= 2 ? viewports.at(1) : nullptr;
        if (!primary || !secondary)
            return fail(QStringLiteral("双视口对象缺失"));
        View3D::ViewportCameraState before;
        if (!secondary->cameraState(&before))
            return fail(QStringLiteral("无法读取第二视口相机"));
        View3D::ViewportCameraState moved;
        if (!primary->cameraState(&moved))
            return fail(QStringLiteral("无法读取主视口相机"));
        moved.position[0] += 1.0;
        if (!primary->setCameraState(moved))
            return fail(QStringLiteral("无法移动主视口相机"));
        waitFor(150);
        View3D::ViewportCameraState after;
        if (!secondary->cameraState(&after))
            return fail(QStringLiteral("无法读取第二视口相机"));
        if (qAbs(after.position[0] - before.position[0]) > 1e-6)
            return fail(QStringLiteral("同步关闭时第二视口仍被主视口带动"));
        cameraSync->trigger();
        uiHost.refreshCommandStates();
        bool followed = false;
        QElapsedTimer syncTimer;
        syncTimer.start();
        while (syncTimer.elapsed() < 2000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            if (secondary->cameraState(&after) &&
                qAbs(after.position[0] - (before.position[0] + 1.0)) < 1e-6) {
                followed = true;
                break;
            }
            QThread::msleep(5);
        }
        if (!followed || !cameraSync->isChecked())
            return fail(QStringLiteral("开启同步后第二视口未跟随主视口"));
        std::cerr << "VIEW3D_CAMERA_SYNC_OK default=off follow=on\n";
        cameraSync->trigger();
        uiHost.refreshCommandStates();
        if (cameraSync->isChecked())
            return fail(QStringLiteral("同步相机关闭失败"));
    }
    if (!switchLayout(QStringLiteral("cmd.viewport.layout_quad"), 4))
        return false;
    if (!switchLayout(QStringLiteral("cmd.viewport.layout_single"), 1))
        return false;
    return true;
}

// 二维（水平面）模式：锁定正俯视、不可旋转、隐藏高程维度，退出恢复进入前的相机。
bool verifyPlanarModeSmoke(View3DApplication::PluginUiHost &uiHost,
                           RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto *view = layout ? layout->primaryView() : nullptr;
    if (!view)
        return fail(QStringLiteral("二维模式检查缺少视口"));
    auto *mode2d = uiHost.actions().value(QStringLiteral("cmd.view.mode_2d"));
    auto *mode3d = uiHost.actions().value(QStringLiteral("cmd.view.mode_3d"));
    if (!mode2d || !mode3d) {
        std::cerr << "VIEW3D_PLANAR_MODE_SKIPPED reason=commands_not_configured" << std::endl;
        return true;
    }
    if (mode2d->isChecked() || !mode3d->isChecked())
        return fail(QStringLiteral("默认视图模式应为三维"));

    View3D::ViewportCameraState baseline;
    if (!view->cameraState(&baseline))
        return fail(QStringLiteral("无法读取基准相机"));

    mode2d->trigger();
    View3D::ViewportCameraState flat;
    if (!view->cameraState(&flat))
        return fail(QStringLiteral("二维模式下无法读取相机"));
    if (!view->planarMode())
        return fail(QStringLiteral("命令未把视口切到二维模式"));
    if (!flat.orthographic)
        return fail(QStringLiteral("二维模式未强制正交投影"));
    // 正俯视：视线沿 -Y（高程轴），屏幕上方是北（-Z）。
    if (qAbs(flat.direction[0]) > 1e-4 || qAbs(flat.direction[1] + 1.0) > 1e-4 ||
        qAbs(flat.direction[2]) > 1e-4)
        return fail(QStringLiteral("二维模式视线不是正向下：%1,%2,%3")
                        .arg(flat.direction[0]).arg(flat.direction[1]).arg(flat.direction[2]));
    if (qAbs(flat.up[0]) > 1e-4 || qAbs(flat.up[1]) > 1e-4 ||
        qAbs(flat.up[2] + 1.0) > 1e-4)
        return fail(QStringLiteral("二维模式上方向不是北：%1,%2,%3")
                        .arg(flat.up[0]).arg(flat.up[1]).arg(flat.up[2]));
    if (!(flat.position[1] > flat.target[1]))
        return fail(QStringLiteral("二维模式相机不在观察点上方"));
    if (!view->scene() || !view->scene()->axesPlanar())
        return fail(QStringLiteral("二维模式未隐藏高程轴"));
    if (!mode2d->isChecked() || mode3d->isChecked())
        return fail(QStringLiteral("二维命令勾选状态未跟随模式"));
    const auto storedViewMode = [] {
        return QSettings(QDir(QApplication::applicationDirPath())
                             .filePath(QStringLiteral("layout_state.ini")),
                         QSettings::IniFormat)
            .value(QStringLiteral("view/mode"))
            .toString();
    };
    if (storedViewMode() != QStringLiteral("2d"))
        return fail(QStringLiteral("二维模式未写入布局状态：%1").arg(storedViewMode()));

    // 切到双视口：新建的视口必须继承二维模式，否则相机同步会把斜视姿态倒灌进来。
    auto *dualAction = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_dual"));
    auto *singleAction = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_single"));
    if (dualAction && singleAction && dualAction->isEnabled()) {
        dualAction->trigger();
        QElapsedTimer layoutWait;
        layoutWait.start();
        while (layoutWait.elapsed() < 4000 && layout->layoutViewportCount() != 2) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const auto planarViews = layout->views();
        if (layout->layoutViewportCount() != 2 || planarViews.size() < 2)
            return fail(QStringLiteral("二维模式下无法切到双视口"));
        for (View3D::IRenderSurface *candidate : planarViews) {
            if (!candidate || !candidate->planarMode())
                return fail(QStringLiteral("新建视口未继承二维模式"));
        }
        singleAction->trigger();
        layoutWait.restart();
        while (layoutWait.elapsed() < 4000 && layout->layoutViewportCount() != 1) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    }

    // 旋转必须被拒绝，且完全不改动相机。
    if (view->orbitCamera(30.0, 30.0))
        return fail(QStringLiteral("二维模式接受了旋转"));
    View3D::ViewportCameraState afterOrbit;
    if (!view->cameraState(&afterOrbit))
        return fail(QStringLiteral("旋转被拒后无法读取相机"));
    if (afterOrbit.position != flat.position || afterOrbit.direction != flat.direction)
        return fail(QStringLiteral("旋转被拒但相机仍被改动"));

    // 立面视角同样被拒绝，俯视图允许（与锁定姿态一致）。
    if (view->setStandardCameraView(View3D::StandardCameraView::Front))
        return fail(QStringLiteral("二维模式接受了立面视角"));
    if (!view->setStandardCameraView(View3D::StandardCameraView::Top))
        return fail(QStringLiteral("二维模式拒绝了俯视图"));
    auto *frontAction = uiHost.actions().value(QStringLiteral("cmd.view.front"));
    if (frontAction && frontAction->isEnabled())
        return fail(QStringLiteral("二维模式下前视图命令仍可用"));

    // 平移只在水平面内生效：观察点高程不变。
    const double targetHeightBefore = flat.target[1];
    if (!view->panCamera(40.0, 40.0))
        return fail(QStringLiteral("二维模式无法平移"));
    View3D::ViewportCameraState panned;
    if (!view->cameraState(&panned))
        return fail(QStringLiteral("平移后无法读取相机"));
    if (qAbs(panned.target[1] - targetHeightBefore) > 1e-6)
        return fail(QStringLiteral("平移改变了高程：%1 → %2")
                        .arg(targetHeightBefore).arg(panned.target[1]));
    if (qAbs(panned.target[0] - flat.target[0]) < 1e-9 &&
        qAbs(panned.target[2] - flat.target[2]) < 1e-9)
        return fail(QStringLiteral("平移没有移动观察点"));

    // 键盘前进（沿视线）不能把相机拽进地面。
    const double cameraHeightBefore = panned.position[1];
    if (!view->moveCameraLocal(0.0, 0.0, 5.0))
        return fail(QStringLiteral("二维模式拒绝了水平移动接口"));
    View3D::ViewportCameraState pushed;
    if (!view->cameraState(&pushed))
        return fail(QStringLiteral("沿视线移动后无法读取相机"));
    if (qAbs(pushed.position[1] - cameraHeightBefore) > 1e-6)
        return fail(QStringLiteral("二维模式沿视线移动改变了相机高程"));

    mode3d->trigger();
    if (view->planarMode())
        return fail(QStringLiteral("三维命令未退出二维模式"));
    if (view->scene()->axesPlanar())
        return fail(QStringLiteral("退出二维后高程轴仍被隐藏"));
    View3D::ViewportCameraState restored;
    if (!view->cameraState(&restored))
        return fail(QStringLiteral("退出二维后无法读取相机"));
    // 原地起身：二维里平移到的观察点必须留住，不能被拉回进入前的位置。
    if (qAbs(restored.target[0] - pushed.target[0]) > 1e-3 ||
        qAbs(restored.target[2] - pushed.target[2]) > 1e-3)
        return fail(QStringLiteral("退出二维丢掉了二维里的观察点：%1,%2 → %3,%4")
                        .arg(pushed.target[0], 0, 'f', 3)
                        .arg(pushed.target[2], 0, 'f', 3)
                        .arg(restored.target[0], 0, 'f', 3)
                        .arg(restored.target[2], 0, 'f', 3));
    if (restored.orthographic != baseline.orthographic)
        return fail(QStringLiteral("退出二维未恢复进入前的投影模式"));
    // 俯仰与方位回到进入前的观察风格。
    for (int axis = 0; axis < 3; ++axis) {
        if (qAbs(restored.direction[axis] - baseline.direction[axis]) > 1e-3)
            return fail(QStringLiteral("退出二维未恢复进入前的视角朝向（轴 %1）").arg(axis));
    }
    // 缩放要接得上：二维里的可见高度折算成同等视野的三维观察距离。
    if (!baseline.orthographic && pushed.orthographicHeight > 0.01 &&
        baseline.verticalFovRadians > 0.01) {
        const double expected = pushed.orthographicHeight /
            (2.0 * std::tan(baseline.verticalFovRadians * 0.5));
        const double dx = restored.position[0] - restored.target[0];
        const double dy = restored.position[1] - restored.target[1];
        const double dz = restored.position[2] - restored.target[2];
        const double actual = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (qAbs(actual - expected) > expected * 0.05 + 1e-3)
            return fail(QStringLiteral("退出二维后缩放没接上：期望距离 %1，实际 %2")
                            .arg(expected, 0, 'f', 3).arg(actual, 0, 'f', 3));
    }
    if (!mode3d->isChecked() || mode2d->isChecked())
        return fail(QStringLiteral("三维命令勾选状态未跟随模式"));
    if (storedViewMode() != QStringLiteral("3d"))
        return fail(QStringLiteral("三维模式未写入布局状态：%1").arg(storedViewMode()));

    // 旋转在三维模式下要能正常工作，证明只是模式差异而非功能坏掉。
    if (!view->orbitCamera(10.0, 0.0))
        return fail(QStringLiteral("三维模式拒绝了旋转"));

    // 多视口：每个当前显示的视口都必须真正离开平面锁定姿态，并且投影模式
    // 各自回到它自己进入二维前的值（隐藏视口保留自己的旧基准是合法的，不要求一致）。
    auto *dualAgain = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_dual"));
    auto *singleAgain = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_single"));
    if (dualAgain && singleAgain && dualAgain->isEnabled()) {
        const auto shownBefore = layout->views();
        QVector<View3D::ViewportCameraState> priorStates;
        for (View3D::IRenderSurface *surface : shownBefore) {
            View3D::ViewportCameraState s;
            if (surface && surface->cameraState(&s))
                priorStates.append(s);
        }
        mode2d->trigger();
        dualAgain->trigger();
        QElapsedTimer grow;
        grow.start();
        while (grow.elapsed() < 4000 && layout->layoutViewportCount() != 2) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        mode3d->trigger();
        singleAgain->trigger();
        QElapsedTimer shrink;
        shrink.start();
        while (shrink.elapsed() < 4000 && layout->layoutViewportCount() != 1) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const int shown = qMin(layout->views().size(), layout->layoutViewportCount());
        if (shown < 1)
            return fail(QStringLiteral("双视口一致性检查后视口数量异常"));
        for (int index = 0; index < shown; ++index) {
            View3D::IRenderSurface *surface = layout->views().at(index);
            if (!surface)
                return fail(QStringLiteral("一致性检查遇到空视口"));
            if (surface->planarMode())
                return fail(QStringLiteral("视口 %1 退出二维后仍处于平面模式").arg(index));
            View3D::ViewportCameraState s;
            if (!surface->cameraState(&s))
                return fail(QStringLiteral("视口 %1 退出二维后无法读取相机").arg(index));
            // 正俯视（dir≈(0,-1,0)）就是"卡在二维"的特征。
            if (qAbs(s.direction[0]) < 1e-3 && qAbs(s.direction[2]) < 1e-3)
                return fail(QStringLiteral("视口 %1 退出二维后卡在正俯视").arg(index));
            if (index < priorStates.size() && s.orthographic != priorStates[index].orthographic)
                return fail(QStringLiteral("视口 %1 退出二维后投影模式与进入前不一致").arg(index));
        }
    }

    std::cerr << "VIEW3D_PLANAR_MODE_OK ortho=1 topdown=1 north_up=1 orbit_rejected=1 "
                 "pan_in_plane=1 position_kept=1 zoom_kept=1 projection_restored=1" << std::endl;
    return true;
}

// GeoPlugin 接线三件事：图层树的状态要真的改动渲染场景；世界原点不能被后加载的
// 数据源搬走；拾取按监听者广播，测量工具进出不能清掉别的插件的监听者。
bool verifyGeoWiringSmoke(View3DApplication::PluginUiHost &uiHost,
                          RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto *view = layout ? layout->primaryView() : nullptr;
    QWidget *widget = view ? view->widget() : nullptr;
    if (!layers || !scene || !view || !widget || widget->width() < 16 ||
        widget->height() < 16)
        return fail(QStringLiteral("地理接线检查缺少图层服务、场景或可用的视口"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };

    // ── 1. 影像图层：勾选与透明度必须落到渲染场景 ──────────────────────────
    const QString imageryKey = QStringLiteral("geo:smoke-imagery");
    if (!scene->addModelLayer(imageryKey, makeSmokeTriangleMesh()))
        return fail(QStringLiteral("无法把影像网格提交到场景"));
    const View3D::LayerId imageryLayer =
        layers->addImageryLayer(QStringLiteral("冒烟影像"), imageryKey);
    if (imageryLayer.isNull()) {
        scene->removeModelLayer(imageryKey);
        return fail(QStringLiteral("影像图层登记失败"));
    }
    if (!layers->setLayerVisible(imageryLayer, false) ||
        scene->isModelLayerVisible(imageryKey)) {
        layers->removeLayer(imageryLayer);
        return fail(QStringLiteral("影像图层隐藏没有落到渲染场景"));
    }
    if (!layers->setLayerVisible(imageryLayer, true) ||
        !scene->isModelLayerVisible(imageryKey)) {
        layers->removeLayer(imageryLayer);
        return fail(QStringLiteral("影像图层显示没有落到渲染场景"));
    }
    if (!layers->setLayerOpacity(imageryLayer, 0.5f) ||
        qAbs(scene->modelLayerOpacity(imageryKey) - 0.5f) > 1e-3) {
        layers->removeLayer(imageryLayer);
        return fail(QStringLiteral("影像图层透明度没有落到渲染场景"));
    }

    // ── 2. 组级可见性：藏起父组要连组里的影像图层一起藏 ────────────────────
    const QString groupedKey = QStringLiteral("geo:smoke-imagery-grouped");
    if (!scene->addModelLayer(groupedKey, makeSmokeTriangleMesh())) {
        layers->removeLayer(imageryLayer);
        return fail(QStringLiteral("无法把组内影像网格提交到场景"));
    }
    const View3D::LayerId group = layers->addGroup(QStringLiteral("冒烟地理组"));
    const View3D::LayerId groupedLayer =
        layers->addImageryLayer(QStringLiteral("冒烟组内影像"), groupedKey, group);
    if (group.isNull() || groupedLayer.isNull()) {
        layers->removeLayer(imageryLayer);
        scene->removeModelLayer(groupedKey);
        return fail(QStringLiteral("组内影像图层登记失败"));
    }
    if (!layers->setLayerVisible(group, false) || scene->isModelLayerVisible(groupedKey)) {
        layers->removeLayer(group);
        layers->removeLayer(imageryLayer);
        return fail(QStringLiteral("组级隐藏没有作用到影像图层"));
    }
    layers->setLayerVisible(group, true);

    // ── 3. 图层删除要回收场景里的网格 ──────────────────────────────────────
    layers->removeLayer(group);
    layers->removeLayer(imageryLayer);
    const QStringList remaining = scene->modelLayerKeys();
    if (remaining.contains(groupedKey) || remaining.contains(imageryKey)) {
        scene->removeModelLayer(groupedKey);
        scene->removeModelLayer(imageryKey);
        return fail(QStringLiteral("删除图层后场景网格没有被回收"));
    }

    // ── 4. 世界原点由第一个数据源钉住，换算结果始终是真实高程 ───────────────
    auto *reference = container->objectAs<View3D::IGeoReference>(
        View3D::ServiceIds::GeoReference);
    if (!reference) {
        std::cerr << "VIEW3D_GEO_WIRING_PARTIAL reason=geo_reference_absent" << std::endl;
    } else {
        reference->resetOrigin();
        if (reference->ready())
            return fail(QStringLiteral("resetOrigin 之后参考系仍然就绪"));
        if (!reference->setOrigin(116.39, 39.91, 0.0))
            return fail(QStringLiteral("首个数据源无法钉住世界原点"));
        if (reference->setOrigin(120.0, 30.0, 0.0))
            return fail(QStringLiteral("世界原点被后加载的数据源搬走"));
        std::array<double, 3> local {};
        if (!reference->lonLatHeightToLocal(116.391, 39.91, 5.0, &local))
            return fail(QStringLiteral("经纬度转局部坐标失败"));
        // 东向 0.001°≈85.5m、北向≈0、高程轴向上：这是 (E,U,-N) 约定的判据。
        // up 不必等于 5.0：椭球各点法线不平行，跨 0.001° 经度后原点的 up 分量
        // 差约 5.7mm（旧等距圆柱近似才会精确等于 5.0）。
        if (!(local[0] > 50.0) || !(local[0] < 120.0) || qAbs(local[1] - 5.0) > 5.0e-3 ||
            qAbs(local[2]) > 5.0e-3)
            return fail(QStringLiteral("局部坐标不符合东-上-北约定：%1,%2,%3")
                            .arg(local[0]).arg(local[1]).arg(local[2]));
        double lon = 0.0;
        double lat = 0.0;
        double height = 0.0;
        if (!reference->localToLonLatHeight(local, &lon, &lat, &height))
            return fail(QStringLiteral("局部坐标转经纬度失败"));
        if (qAbs(lon - 116.391) > 1e-6 || qAbs(lat - 39.91) > 1e-6 ||
            qAbs(height - 5.0) > 1e-6)
            return fail(QStringLiteral("经纬度往返不一致：%1,%2,%3")
                            .arg(lon).arg(lat).arg(height));
        reference->resetOrigin();
        if (reference->ready())
            return fail(QStringLiteral("resetOrigin 之后参考系仍然就绪"));
    }

    // ── 5. 拾取按监听者广播 ────────────────────────────────────────────────
    int hitsA = 0;
    int hitsB = 0;
    const View3D::PickListenerId listenerA = view->addPickListener(
        [&hitsA](const View3D::ViewportPickResult &) { ++hitsA; });
    const View3D::PickListenerId listenerB = view->addPickListener(
        [&hitsB](const View3D::ViewportPickResult &) { ++hitsB; });
    const auto dropListeners = [&] {
        view->removePickListener(listenerA);
        view->removePickListener(listenerB);
    };
    if (listenerA == 0 || listenerB == 0 || listenerA == listenerB) {
        dropListeners();
        return fail(QStringLiteral("拾取监听者注册异常"));
    }
    const auto clickOnce = [&] {
        const QPoint position(widget->width() / 2, widget->height() / 2);
        const QPoint global = widget->mapToGlobal(position);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(position), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position), QPointF(global),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);
        QApplication::processEvents();
    };
    clickOnce();
    if (hitsA != 1 || hitsB != 1) {
        dropListeners();
        return fail(QStringLiteral("拾取没有广播给所有监听者：%1/%2").arg(hitsA).arg(hitsB));
    }

    // 测量工具进出之后，本测试登记的监听者必须还在（旧实现里会被整体清空）。
    auto *measureDistance = uiHost.actions().value(QStringLiteral("cmd.measure.distance"));
    if (measureDistance && measureDistance->isEnabled()) {
        measureDistance->trigger();
        waitFor(150);
        measureDistance->trigger();
        waitFor(150);
        const int beforeMeasure = hitsA;
        clickOnce();
        if (hitsA != beforeMeasure + 1) {
            dropListeners();
            return fail(QStringLiteral("测量工具退出后清掉了其它插件的拾取监听者"));
        }
    }

    // 注销其中一个，另一个继续收。
    if (!view->removePickListener(listenerA)) {
        dropListeners();
        return fail(QStringLiteral("拾取监听者注销失败"));
    }
    const int stoppedA = hitsA;
    const int beforeB = hitsB;
    clickOnce();
    dropListeners();
    if (hitsA != stoppedA || hitsB != beforeB + 1) {
        return fail(QStringLiteral("注销后监听者仍在收事件：A=%1 B=%2/%3")
                        .arg(hitsA).arg(hitsB).arg(beforeB));
    }

    // ── 6. 属性表要有可用的入口 ────────────────────────────────────────────
    // 之前 toggleCommand 指向一个没有被任何 ribbon 项引用的命令，面板永远打不开。
    auto *featureToggle = uiHost.actions().value(QStringLiteral("cmd.window.feature_table"));
    auto *featureDock = uiHost.dockWidget(QStringLiteral("dw.feature_table"));
    if (!featureToggle || !featureDock) {
        std::cerr << "VIEW3D_GEO_WIRING_PARTIAL reason=feature_table_not_configured"
                  << std::endl;
    } else {
        const bool closedBefore = featureDock->isClosed();
        featureToggle->trigger();
        waitFor(50);
        if (featureDock->isClosed() == closedBefore) {
            featureDock->toggleView(!closedBefore);
            return fail(QStringLiteral("属性表开关没有改变停靠面板状态"));
        }
        featureDock->toggleView(closedBefore);
    }

    std::cerr << "VIEW3D_GEO_WIRING_OK layers=1 group=1 origin_pinned=1 pick_broadcast=1"
              << std::endl;
    return true;
}

bool verifyViewCullSmoke(View3DApplication::PluginUiHost &uiHost,
                         RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layerService = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    if (!layout || !scene || !layerService) {
        std::cerr << "VIEW3D_VIEW_CULL_SKIPPED framework_only=1" << std::endl;
        return true;
    }

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    auto *dualAction = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_dual"));
    if (!dualAction || !dualAction->isEnabled())
        return fail(QStringLiteral("双视口命令不可用，无法验证按视口裁剪"));
    dualAction->trigger();
    QElapsedTimer layoutTimer;
    layoutTimer.start();
    while (layoutTimer.elapsed() < 8000 && layout->layoutViewportCount() != 2)
        waitFor(20);
    const auto viewports = layout->views();
    View3D::IRenderSurface *nearView = layout->primaryView();
    View3D::IRenderSurface *farView = viewports.size() >= 2 ? viewports.at(1) : nullptr;
    if (layout->layoutViewportCount() != 2 || !nearView || !farView || !nearView->ready() ||
        !farView->ready()) {
        return fail(QStringLiteral("双视口未就绪，无法验证按视口裁剪"));
    }
    const auto pixelSum = [](const QImage &image) {
        quint64 sum = 0;
        if (image.isNull())
            return sum;
        const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < converted.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(converted.constScanLine(y));
            for (int x = 0; x < converted.width(); ++x)
                sum += static_cast<quint64>(line[x]);
        }
        return sum;
    };

    auto mesh = makeSmokeTriangleMesh();
    for (View3D::MeshVertexDto &vertex : mesh->vertices) {
        vertex.x *= 10.0;
        vertex.y *= 10.0;
    }
    const View3D::LayerId group = layerService->addGroup(QStringLiteral("视口裁剪组"));
    const QString renderKey = QStringLiteral("cull:triangle");
    const View3D::LayerId layer = layerService->addModelLayer(
        QStringLiteral("裁剪图层"), renderKey, mesh, group);
    if (group.isNull() || layer.isNull())
        return fail(QStringLiteral("无法登记视口裁剪测试图层"));

    QElapsedTimer readyTimer;
    readyTimer.start();
    while (readyTimer.elapsed() < 3000 &&
           scene->modelLayerRenderState(renderKey) != View3D::LayerRenderState::Ready)
        waitFor(20);
    if (scene->modelLayerRenderState(renderKey) != View3D::LayerRenderState::Ready) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("视口裁剪测试图层未就绪"));
    }

    if (!layerService->setLayerVisible(layer, false)) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("无法隐藏视口裁剪测试图层"));
    }
    const auto applyCamera = [](View3D::IRenderSurface *view, double distance) {
        View3D::ViewportCameraState state;
        if (!view->cameraState(&state))
            return false;
        state.position = {0.0, 0.0, distance};
        state.direction = {0.0, 0.0, -1.0};
        state.up = {0.0, 1.0, 0.0};
        state.target = {0.0, 0.0, 0.0};
        state.orthographic = false;
        return view->setCameraState(state);
    };
    if (!applyCamera(nearView, 10.0) || !applyCamera(farView, 100.0)) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("无法设置视口相机"));
    }
    waitFor(200);
    const View3D::CaptureOptions options;
    const QSize captureSize(256, 256);
    const quint64 hiddenNear = pixelSum(nearView->captureImageOffscreen(captureSize, options));
    const quint64 hiddenFar = pixelSum(farView->captureImageOffscreen(captureSize, options));
    if (hiddenNear == 0 || hiddenFar == 0) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("离屏截图为空，无法比较视口裁剪"));
    }

    if (!layerService->setLayerVisible(layer, true) ||
        !layerService->setLayerVisibleDistance(layer, 0.0, 60.0)) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("无法设置图层距离范围"));
    }
    waitFor(300);
    const quint64 culledNear = pixelSum(nearView->captureImageOffscreen(captureSize, options));
    const quint64 culledFar = pixelSum(farView->captureImageOffscreen(captureSize, options));

    // 数据层不受渲染裁剪影响：射线拾取仍应命中。
    View3D::ViewportPickResult pick;
    const bool pickable = scene->pickRay({0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, &pick) &&
        pick.renderKey == renderKey;

    if (!layerService->setLayerVisibleDistance(layer, 0.0, 0.0)) {
        layerService->removeLayer(group);
        return fail(QStringLiteral("无法清除图层距离范围"));
    }
    waitFor(300);
    const quint64 unlimitedFar = pixelSum(farView->captureImageOffscreen(captureSize, options));
    layerService->removeLayer(group);
    if (auto *singleAction = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_single")))
        singleAction->trigger();
    waitFor(100);

    if (culledNear == hiddenNear)
        return fail(QStringLiteral("近处视口未显示图层：按视口距离裁剪把可见视口也裁掉了"));
    if (culledFar != hiddenFar)
        return fail(QStringLiteral("远处视口仍显示图层：距离裁剪未按视口生效"));
    if (unlimitedFar == hiddenFar)
        return fail(QStringLiteral("清除距离范围后远处视口仍未恢复显示"));
    if (!pickable)
        return fail(QStringLiteral("渲染裁剪不应影响射线拾取"));
    std::cerr << "VIEW3D_VIEW_CULL_OK nearShown=1 farCulled=1 restored=1 pick=1"
              << std::endl;
    return true;
}

// 把“发黑”变成可量指标的：dark 是被画成近黑的像素比例，
// blackened 是背景本来更亮、加载后变黑的像素（排除天空/主题底色本身的暗）。
struct ShotStats {
    int darkPermille = 0;
    int blackened = 0;
    int meanLum = 0;
    // 屏内不同颜色的数量（每通道量化到 32 级）。用来分辨"顶点色/影像真的上屏"
    // 和"整片是同一个受光灰"——只看 lum 分不出这两者。
    int distinctColours = 0;
};

ShotStats analyseShot(const QImage &shot, const QImage &background)
{
    ShotStats stats;
    if (shot.isNull())
        return stats;
    const QImage image = shot.convertToFormat(QImage::Format_ARGB32);
    const QImage base =
        background.isNull() || background.size() != image.size()
            ? QImage()
            : background.convertToFormat(QImage::Format_ARGB32);
    long long sum = 0;
    int dark = 0;
    int blackened = 0;
    int total = 0;
    QSet<int> colours;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const auto *baseLine =
            base.isNull() ? nullptr : reinterpret_cast<const QRgb *>(base.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int lum = qRed(line[x]) + qGreen(line[x]) + qBlue(line[x]);
            sum += lum;
            ++total;
            colours.insert((qRed(line[x]) >> 3) << 10 | (qGreen(line[x]) >> 3) << 5 |
                           (qBlue(line[x]) >> 3));
            if (lum < 24)
                ++dark;
            if (baseLine && lum < 24) {
                const int baseLum =
                    qRed(baseLine[x]) + qGreen(baseLine[x]) + qBlue(baseLine[x]);
                if (baseLum >= 60)
                    ++blackened;
            }
        }
    }
    if (total > 0) {
        stats.darkPermille = dark * 1000 / total;
        stats.meanLum = static_cast<int>(sum / total);
    }
    stats.distinctColours = colours.size();
    stats.blackened = blackened;
    return stats;
}

// 画面覆盖判据：屏内不该出现沿瓦片轮廓的空白。
// 空白分两类：天空与数据集范围外是"本来就没有数据"，footprint 内才是"该有数据却没显示"。
// 掩码由包围盒底面四边形投影得到（见 buildFootprintMask），判据只落在应当有数据的区域。
// 背景色按行取自空场景参考图：天空是屏幕空间的竖直渐变，与相机姿态无关，
// 所以一张参考图可服务同尺寸的所有视角。
struct CoverageStats {
    bool valid = false;
    int blankPermille = 0;     // 全图背景占比（含天空，只作对照）
    int holePermille = 0;      // 全图封闭空白占比（逃不到屏幕边的那部分）
    int footBlankPermille = 0; // 主判据：footprint 内的背景占比
    int footPermille = 0;      // 掩码自身覆盖率：footprint 占整屏多少，太小时主判据不可信
    int edgeMean = 0;          // 掩码内的平均横向亮度梯度：空白占比为 0 也可能是满屏碎片
    int badCells = 0;          // footprint 内空白过半的格子数
    int judgedCells = 0;       // 落在 footprint 内的格子总数
    QByteArray map;            // '-'=不判定 '.'=覆盖良好 1-8=空白十分位 '#'=全空
};

// 把数据集包围盒的底面四边形投影到屏幕，得到"这里应当有数据"的掩码。
// 相机站在这块矩形上方时（近景很常见）多边形会绕到相机背后，所以先在相机空间
// 按近平面裁剪再投影；整个矩形都在背后时返回全 0 掩码，表示本视角没有可判定的地面。
std::vector<uint8_t> buildFootprintMask(const View3D::ViewportCameraState &camera,
                                        const QSize &size, double groundY,
                                        const std::array<double, 3> &minimum,
                                        const std::array<double, 3> &maximum)
{
    const int width = size.width();
    const int height = size.height();
    if (camera.orthographic || width < 8 || height < 8)
        return {};
    const auto normalize = [](std::array<double, 3> v) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-9)
            return std::array<double, 3> {0.0, 0.0, 0.0};
        return std::array<double, 3> {v[0] / n, v[1] / n, v[2] / n};
    };
    const std::array<double, 3> forward = normalize(camera.direction);
    if (forward[0] == 0.0 && forward[1] == 0.0 && forward[2] == 0.0)
        return {};
    const auto cross = [](const std::array<double, 3> &a, const std::array<double, 3> &b) {
        return std::array<double, 3> {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                      a[0] * b[1] - a[1] * b[0]};
    };
    const std::array<double, 3> right = normalize(cross(forward, camera.up));
    if (right[0] == 0.0 && right[1] == 0.0 && right[2] == 0.0)
        return {};
    const std::array<double, 3> up = normalize(cross(right, forward));
    const double halfTanH = std::tan(camera.horizontalFovRadians * 0.5);
    const double halfTanV = std::tan(camera.verticalFovRadians * 0.5);
    if (halfTanH < 1e-6 || halfTanV < 1e-6)
        return {};
    std::vector<uint8_t> mask(size_t(width) * height, 0);

    std::array<std::array<double, 3>, 4> poly {};
    const double xs[4] = {minimum[0], maximum[0], maximum[0], minimum[0]};
    const double zs[4] = {minimum[2], minimum[2], maximum[2], maximum[2]};
    for (int i = 0; i < 4; ++i) {
        const std::array<double, 3> delta {xs[i] - camera.position[0],
                                           groundY - camera.position[1],
                                           zs[i] - camera.position[2]};
        poly[i] = {delta[0] * right[0] + delta[1] * up[0] + delta[2] * forward[0],
                   delta[0] * right[1] + delta[1] * up[1] + delta[2] * forward[1],
                   delta[0] * right[2] + delta[1] * up[2] + delta[2] * forward[2]};
    }
    // 近平面裁剪，近平面取 0.5 m：再小投影坐标会爆。
    const double zNear = 0.5;
    std::vector<std::array<double, 3>> clipped;
    for (size_t i = 0; i < poly.size(); ++i) {
        const std::array<double, 3> &a = poly[i];
        const std::array<double, 3> &b = poly[(i + 1) % poly.size()];
        const bool aIn = a[2] >= zNear;
        if (aIn)
            clipped.push_back(a);
        const bool bIn = b[2] >= zNear;
        if (aIn == bIn)
            continue;
        const double t = (zNear - a[2]) / (b[2] - a[2]);
        clipped.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, zNear});
    }
    if (clipped.size() < 3)
        return mask; // 本视角看不到数据集地面：没有可判定的区域
    std::vector<std::array<double, 2>> screen;
    screen.reserve(clipped.size());
    for (const auto &p : clipped)
        screen.push_back({(p[0] / p[2] / halfTanH * 0.5 + 0.5) * width,
                          (0.5 - p[1] / p[2] / halfTanV * 0.5) * height});
    for (int y = 0; y < height; ++y) {
        const double py = y + 0.5;
        std::vector<double> crossings;
        for (size_t i = 0; i < screen.size(); ++i) {
            const std::array<double, 2> &a = screen[i];
            const std::array<double, 2> &b = screen[(i + 1) % screen.size()];
            if ((a[1] > py) == (b[1] > py))
                continue;
            crossings.push_back(a[0] + (py - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
        }
        std::sort(crossings.begin(), crossings.end());
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            const int x0 = std::max(0, int(std::floor(crossings[i])));
            const int x1 = std::min(width - 1, int(std::ceil(crossings[i + 1])));
            for (int x = x0; x <= x1; ++x)
                mask[size_t(y) * width + x] = 1;
        }
    }
    return mask;
}

CoverageStats analyseCoverage(const QImage &shot, const QImage &background,
                              const std::vector<uint8_t> &mask, int grid = 16)
{
    CoverageStats stats;
    if (shot.isNull() || shot.width() < grid * 2 || shot.height() < grid * 2 ||
        background.isNull() || background.size() != shot.size())
        return stats;
    stats.valid = true;
    const QImage image = shot.convertToFormat(QImage::Format_ARGB32);
    const QImage base = background.convertToFormat(QImage::Format_ARGB32);
    const int width = image.width();
    const int height = image.height();
    std::vector<QRgb> rowBase(height);
    for (int y = 0; y < height; ++y)
        rowBase[y] = reinterpret_cast<const QRgb *>(base.constScanLine(y))[0];

    const bool haveMask = mask.size() == size_t(width) * height;
    std::vector<uint8_t> blank(size_t(width) * height, 0);
    int blankCount = 0;
    long long edgeSum = 0;
    int edgeCount = 0;
    int footPixels = 0;
    int footBlankCount = 0;
    for (int y = 0; y < height; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const QRgb baseRgb = rowBase[y];
        for (int x = 0; x < width; ++x) {
            const size_t index = size_t(y) * width + x;
            const bool inside = !haveMask || mask[index] != 0;
            if (inside) {
                ++footPixels;
                if (x + 1 < width && (!haveMask || mask[index + 1] != 0)) {
                    const int lum = qRed(line[x]) + qGreen(line[x]) + qBlue(line[x]);
                    const QRgb next = line[x + 1];
                    edgeSum += std::abs(lum - (qRed(next) + qGreen(next) + qBlue(next)));
                    ++edgeCount;
                }
            }
            if (std::abs(qRed(line[x]) - qRed(baseRgb)) > 4 ||
                std::abs(qGreen(line[x]) - qGreen(baseRgb)) > 4 ||
                std::abs(qBlue(line[x]) - qBlue(baseRgb)) > 4)
                continue;
            blank[index] = 1;
            ++blankCount;
            if (inside)
                ++footBlankCount;
        }
    }

    // 从四条边界出发洪水填充（8 邻接）：能逃到屏幕边的空白不算洞。
    std::vector<uint8_t> escape(size_t(width) * height, 0);
    std::vector<int> stack;
    int escaped = 0;
    const auto claim = [&](int x, int y) {
        const size_t index = size_t(y) * width + x;
        if (!blank[index] || escape[index])
            return;
        escape[index] = 1;
        ++escaped;
        stack.push_back(int(index));
    };
    for (int x = 0; x < width; ++x) {
        claim(x, 0);
        claim(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        claim(0, y);
        claim(width - 1, y);
    }
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const int x = index % width;
        const int y = index / width;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx >= 0 && nx < width && ny >= 0 && ny < height)
                    claim(nx, ny);
            }
        }
    }
    const int holeCount = blankCount - escaped;
    const int total = width * height;
    stats.blankPermille = blankCount * 1000 / total;
    stats.holePermille = std::max(0, holeCount) * 1000 / total;
    stats.footBlankPermille = footPixels > 0 ? footBlankCount * 1000 / footPixels : 0;
    stats.footPermille = footPixels * 1000 / total;
    stats.edgeMean = edgeCount > 0 ? int(edgeSum / edgeCount) : 0;

    const int cellWidth = width / grid;
    const int cellHeight = height / grid;
    for (int gy = 0; gy < grid; ++gy) {
        for (int gx = 0; gx < grid; ++gx) {
            int cellBlank = 0;
            int cellTotal = 0;
            int cellFoot = 0;
            for (int y = gy * cellHeight; y < (gy + 1) * cellHeight; ++y) {
                for (int x = gx * cellWidth; x < (gx + 1) * cellWidth; ++x) {
                    const size_t index = size_t(y) * width + x;
                    ++cellTotal;
                    cellBlank += blank[index];
                    cellFoot += !haveMask || mask[index] != 0;
                }
            }
            if (cellTotal == 0 || cellFoot * 2 < cellTotal) {
                stats.map.append('-'); // 大半在 footprint 外：天空或数据之外，不判定
                continue;
            }
            ++stats.judgedCells;
            const int permille = cellBlank * 1000 / cellTotal;
            char mark = '.';
            if (permille >= 950)
                mark = '#';
            else if (permille >= 50)
                mark = char('0' + std::min(9, permille / 100));
            stats.map.append(mark);
            if (permille > 500)
                ++stats.badCells;
        }
        if (gy + 1 < grid)
            stats.map.append('/');
    }
    return stats;
}

struct SceneTreeMenuProbe {
    bool shown = false;
    QStringList actions;
    QString triggered;
    int iconActions = 0;
};

QPoint sceneTreeItemMenuPoint(QTreeWidget *tree, QTreeWidgetItem *item)
{
    if (!tree || !item)
        return {};
    tree->expandAll();
    tree->scrollToItem(item);
    QApplication::processEvents();
    QRect rect = tree->visualItemRect(item);
    if (rect.isEmpty()) {
        tree->resize(420, 640);
        QApplication::processEvents();
        rect = tree->visualItemRect(item);
    }
    return rect.center();
}

// 无头环境下直接触发 customContextMenuRequested，菜单 exec 期间用定时器读取条目并关闭。
// 这样既不依赖桌面截图，也能验证真实弹出的菜单内容与动作。
SceneTreeMenuProbe probeSceneTreeMenu(QTreeWidget *tree, const QPoint &position,
                                      const QString &triggerText = {})
{
    SceneTreeMenuProbe probe;
    QTimer inspector;
    inspector.setSingleShot(true);
    inspector.setInterval(0);
    QObject::connect(&inspector, &QTimer::timeout, [&probe, triggerText] {
        QMenu *menu = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *candidate = qobject_cast<QMenu *>(widget);
            if (candidate && candidate->isVisible()) {
                menu = candidate;
                break;
            }
        }
        if (!menu)
            return;
        probe.shown = true;
        for (QAction *action : menu->actions()) {
            if (action->isSeparator())
                continue;
            probe.actions.append(action->text());
            if (!action->icon().isNull())
                ++probe.iconActions;
            if (!triggerText.isEmpty() && action->text() == triggerText) {
                probe.triggered = action->text();
                action->trigger();
            }
        }
        menu->close();
    });
    inspector.start();
    tree->customContextMenuRequested(position);
    inspector.stop();
    return probe;
}

bool menuHasActions(const SceneTreeMenuProbe &probe, const QStringList &expected,
                    QString *missing)
{
    for (const QString &text : expected) {
        if (!probe.actions.contains(text)) {
            if (missing)
                *missing = text;
            return false;
        }
    }
    return true;
}

struct ViewportMenuProbe {
    bool shown = false;
    QStringList actions;
    QString triggered;
    int iconActions = 0;
};

// 模拟一次“右键按下→抬起（不拖动）”，在菜单 exec 期间读取条目并触发/关闭。
// 右键拖拽是轨道旋转，这里刻意不发送 MouseMove，验证的是工具菜单而不是旋转。
ViewportMenuProbe probeViewportMenu(View3D::IRenderSurface *view, const QPoint &position,
                                    const QString &triggerText = {})
{
    ViewportMenuProbe probe;
    if (!view || !view->widget())
        return probe;
    QWidget *widget = view->widget();
    const QPoint global = widget->mapToGlobal(position);
    QTimer inspector;
    inspector.setInterval(10);
    bool handled = false;
    QObject::connect(&inspector, &QTimer::timeout, [&probe, &handled, &inspector,
                                                    triggerText] {
        if (handled)
            return;
        QMenu *menu = nullptr;
        for (QWidget *candidateWidget : QApplication::topLevelWidgets()) {
            auto *candidate = qobject_cast<QMenu *>(candidateWidget);
            if (candidate && candidate->isVisible()) {
                menu = candidate;
                break;
            }
        }
        if (!menu)
            return;
        handled = true;
        probe.shown = true;
        for (QAction *action : menu->actions()) {
            if (action->isSeparator())
                continue;
            probe.actions.append(action->text());
            if (!action->icon().isNull())
                ++probe.iconActions;
            if (!triggerText.isEmpty() && action->text() == triggerText) {
                probe.triggered = action->text();
                action->trigger();
            }
        }
        inspector.stop();
        menu->close();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        if (QWidget *popup = QApplication::activePopupWidget())
            popup->close();
    });
    watchdog.start(2000);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(position), QPointF(global),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position), QPointF(global),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    inspector.start();
    QApplication::processEvents(QEventLoop::AllEvents, 100);
    inspector.stop();
    watchdog.stop();
    return probe;
}

bool verifyMeasureSmoke(View3DApplication::PluginUiHost &uiHost,
                        RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    bool toolsConfigured = false;
    bool toolsLoaded = false;
    if (auto *packageRegistry = container
            ? container->objectAs<RF::PluginEngine::IPluginRegistryService>(
                  RF::PluginEngine::PluginRegistryServiceId)
            : nullptr) {
        for (const RF::PluginEngine::PluginPackageStatus &status :
             packageRegistry->packages()) {
            if (status.id == QStringLiteral("plugin.view3d.tools")) {
                toolsConfigured = true;
                toolsLoaded = status.loaded;
            }
        }
    }
    if (toolsConfigured && !toolsLoaded)
        return fail(QStringLiteral("基础工具插件已配置但未加载"));
    auto *distanceCommand = uiHost.actions().value(QStringLiteral("cmd.measure.distance"));
    if (!toolsConfigured && !distanceCommand) {
        std::cerr << "VIEW3D_MEASURE_SKIPPED reason=plugin_absent\n";
        return true;
    }
    auto *coordinateCommand =
        uiHost.actions().value(QStringLiteral("cmd.measure.coordinate"));
    auto *polylineCommand = uiHost.actions().value(QStringLiteral("cmd.measure.polyline"));
    auto *snappingCommand = uiHost.actions().value(QStringLiteral("cmd.measure.snapping"));
    auto *dockCommand = uiHost.actions().value(QStringLiteral("cmd.window.measurements"));
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *ui = application.getUiObjectRegistry();
    auto *measureDock = ui ? ui->widget(QStringLiteral("dw.measurements")) : nullptr;
    auto *table = measureDock
        ? measureDock->findChild<QTableWidget *>(QStringLiteral("measure.table"))
        : nullptr;
    auto *hint = measureDock
        ? measureDock->findChild<QLabel *>(QStringLiteral("measure.hint"))
        : nullptr;
    if (!coordinateCommand || !polylineCommand || !snappingCommand || !dockCommand ||
        !layout || !layers || !scene || !measureDock || !table || !hint) {
        return fail(QStringLiteral("测量插件服务或结果面板不完整"));
    }
    View3D::IRenderSurface *view = layout->primaryView();
    if (!view || !view->widget() || !view->ready())
        return fail(QStringLiteral("测量测试视口不可用"));
    if (!distanceCommand->isEnabled())
        return fail(QStringLiteral("测量命令未启用"));

    QElapsedTimer timer;
    const auto waitFor = [](int milliseconds) {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    const auto waitUntil = [&](int milliseconds, const std::function<bool()> &pending) {
        timer.restart();
        while (timer.elapsed() < milliseconds && !pending()) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        return pending();
    };
    if (layout->layoutViewportCount() != 1) {
        if (auto *single = uiHost.actions().value(QStringLiteral("cmd.viewport.layout_single")))
            single->trigger();
        waitUntil(3000, [&] { return layout->layoutViewportCount() == 1; });
        view = layout->primaryView();
        if (layout->layoutViewportCount() != 1 || !view || !view->widget() || !view->ready())
            return fail(QStringLiteral("无法恢复单视口进行测量"));
    }
    if (view->widget()->width() <= 16 || view->widget()->height() <= 16)
        return fail(QStringLiteral("测量测试视口尺寸无效"));

    auto mesh = makeSmokeTriangleMesh();
    for (View3D::MeshVertexDto &vertex : mesh->vertices) {
        vertex.x *= 10.0;
        vertex.y *= 10.0;
    }
    const View3D::LayerId group = layers->addGroup(QStringLiteral("测量测试组"));
    const QString renderKey = QStringLiteral("measure:triangle");
    const View3D::LayerId layerId = layers->addModelLayer(
        QStringLiteral("测量测试图层"), renderKey, mesh, group);
    if (group.isNull() || layerId.isNull())
        return fail(QStringLiteral("无法登记测量测试图层"));
    const auto cleanup = [&] {
        layers->removeLayer(group);
        if (measureDock)
            measureDock->hide();
    };
    if (!waitUntil(3000, [&] {
            return scene->modelLayerRenderState(renderKey) ==
                View3D::LayerRenderState::Ready;
        })) {
        cleanup();
        return fail(QStringLiteral("测量测试图层未就绪"));
    }

    View3D::ViewportCameraState camera;
    if (!view->cameraState(&camera)) {
        cleanup();
        return fail(QStringLiteral("无法读取测量测试相机"));
    }
    camera.position = {2.5, 2.5, 30.0};
    camera.target = {2.5, 2.5, 0.0};
    camera.direction = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.orthographic = false;
    if (!view->setCameraState(camera)) {
        cleanup();
        return fail(QStringLiteral("无法设置测量测试相机"));
    }
    waitFor(100);
    View3D::CaptureOptions overlayCapture;
    overlayCapture.includeMeasurementOverlay = true;
    const QWidget *viewWidget = view->widget();
    const qreal viewDpr = viewWidget ? viewWidget->devicePixelRatioF() : 1.0;
    const QSize overlayShotSize(
        qMax(64, qRound((viewWidget ? viewWidget->width() : 256) * viewDpr)),
        qMax(64, qRound((viewWidget ? viewWidget->height() : 256) * viewDpr)));
    const QImage baselineShot =
        view->captureImageOffscreen(overlayShotSize, overlayCapture);
    QWidget *widget = view->widget();
    const auto clickAt = [&](const QPoint &position) {
        const QPoint global = widget->mapToGlobal(position);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(position), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position), QPointF(global),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
        QApplication::processEvents();
    };
    const QSize viewSize = widget->size();
    const QPoint firstPoint(viewSize.width() / 2, viewSize.height() / 2);
    const QPoint secondPoint(qMin(viewSize.width() - 2, firstPoint.x() + 8),
                             qMin(viewSize.height() - 2, firstPoint.y() + 8));

    distanceCommand->trigger();
    QApplication::processEvents();
    if (!distanceCommand->isChecked()) {
        cleanup();
        return fail(QStringLiteral("直线距离命令未进入勾选状态"));
    }
    if (!hint->text().contains(QStringLiteral("直线距离"))) {
        cleanup();
        return fail(QStringLiteral("测量提示未显示当前工具：%1").arg(hint->text()));
    }
    clickAt(firstPoint);
    clickAt(secondPoint);
    if (table->rowCount() != 1 || !table->item(0, 1) ||
        !table->item(0, 1)->text().contains(QStringLiteral("距离"))) {
        std::cerr << "VIEW3D_MEASURE_DIAGNOSTIC hint=" << hint->text().toStdString()
                  << " rows=" << table->rowCount() << std::endl;
        cleanup();
        return fail(QStringLiteral("直线距离未生成结果"));
    }
    if (table->horizontalHeader()->sectionResizeMode(2) != QHeaderView::Stretch ||
        table->horizontalHeader()->stretchLastSection()) {
        cleanup();
        return fail(QStringLiteral("测量结果表仍按长坐标内容扩展列宽"));
    }
    if (!table->item(0, 2) || table->item(0, 2)->toolTip().isEmpty()) {
        cleanup();
        return fail(QStringLiteral("测量结果缺少完整坐标提示"));
    }
    if (!view->hasOverlay()) {
        cleanup();
        return fail(QStringLiteral("直线距离未显示叠加层"));
    }
    waitFor(500);
    const QImage overlayShot =
        view->captureImageOffscreen(overlayShotSize, overlayCapture);
    int overlayPixels = 0;
    if (!baselineShot.isNull() && baselineShot.size() == overlayShot.size()) {
        const QImage before = baselineShot.convertToFormat(QImage::Format_ARGB32);
        const QImage after = overlayShot.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < before.height(); ++y) {
            const auto *beforeLine =
                reinterpret_cast<const QRgb *>(before.constScanLine(y));
            const auto *afterLine =
                reinterpret_cast<const QRgb *>(after.constScanLine(y));
            for (int x = 0; x < before.width(); ++x) {
                if (beforeLine[x] != afterLine[x])
                    ++overlayPixels;
            }
        }
    }
    if (overlayPixels == 0) {
        cleanup();
        return fail(QStringLiteral("测量辅助线没有渲染到视口"));
    }
    std::cerr << "VIEW3D_MEASURE_DISTANCE_OK row=1 overlay=1 checked=1 pixels="
              << overlayPixels << "\n";

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(widget, &escape);
    QApplication::processEvents();
    if (distanceCommand->isChecked()) {
        cleanup();
        return fail(QStringLiteral("Esc 未退出测量工具"));
    }
    if (view->hasOverlay()) {
        cleanup();
        return fail(QStringLiteral("Esc 后测量叠加层未清除"));
    }
    std::cerr << "VIEW3D_MEASURE_ESC_OK\n";

    polylineCommand->trigger();
    QApplication::processEvents();
    if (!polylineCommand->isChecked()) {
        cleanup();
        return fail(QStringLiteral("折线距离命令未进入勾选状态"));
    }
    clickAt(firstPoint);
    clickAt(secondPoint);
    const ViewportMenuProbe finishMenu = probeViewportMenu(
        view, firstPoint, QStringLiteral("完成折线（已拾取 2 个点）"));
    if (!finishMenu.shown) {
        cleanup();
        return fail(QStringLiteral("测量工具激活时视口右键菜单未弹出"));
    }
    if (!finishMenu.actions.contains(QStringLiteral("退出「折线距离」（Esc）"))) {
        cleanup();
        return fail(QStringLiteral("视口右键菜单缺少退出工具项"));
    }
    if (finishMenu.triggered.isEmpty()) {
        std::cerr << "VIEW3D_MEASURE_DIAGNOSTIC menu_actions=";
        for (const QString &action : finishMenu.actions)
            std::cerr << action.toStdString() << " | ";
        std::cerr << std::endl;
        cleanup();
        return fail(QStringLiteral("视口右键菜单无法完成折线"));
    }
    if (finishMenu.iconActions < 2) {
        cleanup();
        return fail(QStringLiteral("视口右键菜单图标不足：%1").arg(finishMenu.iconActions));
    }
    QApplication::processEvents();
    if (table->rowCount() != 2 || !table->item(1, 1) ||
        !table->item(1, 1)->text().contains(QStringLiteral("折线"))) {
        cleanup();
        return fail(QStringLiteral("完成折线后未生成折线结果"));
    }
    std::cerr << "VIEW3D_MEASURE_MENU_OK finish=1\n";

    const ViewportMenuProbe exitMenu = probeViewportMenu(
        view, firstPoint, QStringLiteral("退出「折线距离」（Esc）"));
    if (exitMenu.triggered.isEmpty()) {
        cleanup();
        return fail(QStringLiteral("视口右键菜单无法退出工具"));
    }
    if (exitMenu.iconActions < 1) {
        cleanup();
        return fail(QStringLiteral("视口退出菜单缺少图标"));
    }
    QApplication::processEvents();
    if (polylineCommand->isChecked() || view->hasOverlay()) {
        cleanup();
        return fail(QStringLiteral("右键退出后工具或叠加层未清理"));
    }
    std::cerr << "VIEW3D_MEASURE_EXIT_MENU_OK\n";

    const bool snappingBefore = snappingCommand->isChecked();
    snappingCommand->trigger();
    QApplication::processEvents();
    if (snappingCommand->isChecked() == snappingBefore) {
        cleanup();
        return fail(QStringLiteral("测量吸附开关未切换"));
    }
    snappingCommand->trigger();
    QApplication::processEvents();
    if (snappingCommand->isChecked() != snappingBefore) {
        cleanup();
        return fail(QStringLiteral("测量吸附开关未恢复"));
    }
    std::cerr << "VIEW3D_MEASURE_SNAPPING_OK\n";

    table->cellClicked(0, 0);
    QApplication::processEvents();
    const bool panelShown = view->hasOverlay();
    table->cellClicked(0, 0);
    QApplication::processEvents();
    if (!panelShown || view->hasOverlay()) {
        cleanup();
        return fail(QStringLiteral("测量结果面板点选显隐未生效"));
    }
    std::cerr << "VIEW3D_MEASURE_PANEL_OK toggle=1\n";

    // 固定像素尺寸：同一个辅助点在不同视距下的屏幕面积应基本不变。
    const auto countOrange = [](const QImage &image) {
        if (image.isNull())
            return 0;
        const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
        int count = 0;
        for (int y = 0; y < converted.height(); ++y) {
            const auto *line =
                reinterpret_cast<const QRgb *>(converted.constScanLine(y));
            for (int x = 0; x < converted.width(); ++x) {
                const QRgb colour = line[x];
                if (qRed(colour) > 200 && qGreen(colour) > 100 && qBlue(colour) < 120)
                    ++count;
            }
        }
        return count;
    };
    coordinateCommand->trigger();
    QApplication::processEvents();
    if (!coordinateCommand->isChecked()) {
        cleanup();
        return fail(QStringLiteral("坐标拾取命令未进入勾选状态"));
    }
    clickAt(firstPoint);
    waitFor(300);
    const QImage markerNearShot =
        view->captureImageOffscreen(overlayShotSize, overlayCapture);
    const int markerNearPixels = countOrange(markerNearShot);
    if (markerNearPixels == 0) {
        cleanup();
        return fail(QStringLiteral("坐标测量辅助点没有渲染到视口"));
    }
    View3D::ViewportCameraState closeCamera = camera;
    closeCamera.position = {2.5, 2.5, 15.0};
    closeCamera.target = {2.5, 2.5, 0.0};
    closeCamera.direction = {0.0, 0.0, -1.0};
    closeCamera.up = {0.0, 1.0, 0.0};
    if (!view->setCameraState(closeCamera)) {
        cleanup();
        return fail(QStringLiteral("无法设置测量缩放相机"));
    }
    waitFor(300);
    const QImage markerCloseShot =
        view->captureImageOffscreen(overlayShotSize, overlayCapture);
    const int markerClosePixels = countOrange(markerCloseShot);
    if (markerClosePixels == 0) {
        cleanup();
        return fail(QStringLiteral("缩放后坐标测量辅助点消失"));
    }
    if (markerClosePixels > markerNearPixels * 2 ||
        markerNearPixels > markerClosePixels * 2) {
        cleanup();
        return fail(QStringLiteral("测量辅助点尺寸随缩放变化：近 %1 像素，远 %2 像素")
                        .arg(markerClosePixels)
                        .arg(markerNearPixels));
    }
    std::cerr << "VIEW3D_MEASURE_PIXEL_SIZE_OK near=" << markerNearPixels
              << " close=" << markerClosePixels << "\n";
    QKeyEvent coordinateEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(widget, &coordinateEscape);
    QApplication::processEvents();
    if (coordinateCommand->isChecked() || view->hasOverlay()) {
        cleanup();
        return fail(QStringLiteral("坐标测量退出后未清理"));
    }

    cleanup();
    waitFor(100);
    std::cerr << "VIEW3D_MEASURE_OK distance=1 esc=1 polyline=1 menu=1 snapping=1\n";
    return true;
}


// LAS 1.2 点格式 2（带 RGB）三个点，裸字节写出来：宿主侧不链 LASzip，只需要
// 文件本身能被探测（"LASF" 魔数）并解出坐标。缩放 0.001，所以整数按毫米存。
QByteArray buildLasPointCloudFixture()
{
    constexpr int headerBytes = 227;
    constexpr int recordBytes = 26;
    QByteArray data;
    data.fill('\0', headerBytes + 3 * recordBytes);
    char *base = data.data();
    const auto put16 = [base](int offset, quint16 value) {
        std::memcpy(base + offset, &value, sizeof(value));
    };
    const auto put32 = [base](int offset, quint32 value) {
        std::memcpy(base + offset, &value, sizeof(value));
    };
    const auto putI32 = [base](int offset, qint32 value) {
        std::memcpy(base + offset, &value, sizeof(value));
    };
    const auto put64 = [base](int offset, double value) {
        std::memcpy(base + offset, &value, sizeof(value));
    };
    data.replace(0, 4, QByteArrayLiteral("LASF"));
    base[24] = 1;  // 版本主号
    base[25] = 2;  // 版本副号
    put16(94, headerBytes);
    put32(96, headerBytes);  // 点到点数据偏移（无 VLR）
    put32(107, 3);
    base[104] = 2;  // 点记录格式 2：带 RGB
    put16(105, recordBytes);
    put64(131, 0.001);  // x 缩放
    put64(139, 0.001);  // y 缩放
    put64(147, 0.001);  // z 缩放
    put64(179, 2.0);    // max x
    put64(195, 1.0);    // max y
    put64(211, 1.0);    // max z
    for (int i = 0; i < 3; ++i) {
        const int record = headerBytes + i * recordBytes;
        putI32(record + 0, i * 1000);  // 1 米一步进
        put16(record + 12, static_cast<quint16>(1000 * (i + 1)));
        put16(record + 20, 0xFF00);  // red  = 255
        put16(record + 22, 0x8000);  // green = 128
    }
    return data;
}

// 要素属性表冒烟：注册一个自带要素表的网格（与二维矢量导入、3D Tiles batch table
// 同一个 DTO），走真实左键拾取 → 高亮 → 属性表落数据 → 删除图层后自动清空。
// 属性插件是可选包，面板不存在时按跳过处理。
bool verifyFeatureTableSmoke(View3DApplication::PluginUiHost &uiHost,
                             RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *dockWidget = uiHost.dockWidget(QStringLiteral("dw.feature_table"));
    if (!dockWidget) {
        std::cerr << "VIEW3D_FEATURE_TABLE_SKIPPED reason=plugin_not_loaded" << std::endl;
        return true;
    }
    auto *container = application.getPluginContainer();
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    View3D::IRenderSurface *view = layout ? layout->primaryView() : nullptr;
    auto *table = dockWidget->findChild<QTableWidget *>(QStringLiteral("feature.table"));
    auto *title = dockWidget->findChild<QLabel *>(QStringLiteral("feature.title"));
    if (!scene || !layers || !view || !table || !title)
        return fail(QStringLiteral("属性表链路缺少场景/图层服务/视口或表格"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };

    const QString renderKey = QStringLiteral("feature:smoke");
    const QString layerName = QStringLiteral("属性表冒烟图层");
    auto mesh = makeSmokeTriangleMesh();
    for (View3D::MeshVertexDto &vertex : mesh->vertices) {
        vertex.x *= 10.0;
        vertex.y *= 10.0;
    }
    View3D::ModelPrimitiveDto &primitive = mesh->primitives.front();
    primitive.featureTableIndex = 0;
    primitive.featureIds = {0, 0, 0};
    primitive.doubleSided = true;
    View3D::ModelFeatureTableDto featureTable;
    featureTable.name = QStringLiteral("烟测表");
    featureTable.featureCount = 1;
    for (const auto& pair : {std::pair<QString, QString>{QStringLiteral("名称"),
                                                        QStringLiteral("烟测要素")},
                             {QStringLiteral("面积"), QStringLiteral("12.5")}}) {
        View3D::ModelFeaturePropertyDto property;
        property.id = pair.first;
        property.name = pair.first;
        property.type = QStringLiteral("String");
        property.values = {pair.second};
        featureTable.properties.push_back(property);
    }
    mesh->featureTables.push_back(featureTable);

    const View3D::LayerId group = layers->addGroup(QStringLiteral("属性表冒烟组"));
    const View3D::LayerId layer =
        layers->addModelLayer(layerName, renderKey, mesh, group);
    const auto cleanup = [&] {
        if (!group.isNull())
            layers->removeLayer(group);
        else
            layers->removeLayer(layer);
    };
    if (layer.isNull())
        return fail(QStringLiteral("无法登记属性表冒烟图层"));

    QElapsedTimer ready;
    ready.start();
    while (ready.elapsed() < 3000 &&
           scene->modelLayerRenderState(renderKey) != View3D::LayerRenderState::Ready)
        waitFor(20);
    if (scene->modelLayerRenderState(renderKey) != View3D::LayerRenderState::Ready) {
        cleanup();
        return fail(QStringLiteral("属性表冒烟图层未就绪"));
    }

    // 正对三角形重心的一条视线：重心在三角形内部，拾取点确定。
    View3D::ViewportCameraState camera;
    if (!view->cameraState(&camera)) {
        cleanup();
        return fail(QStringLiteral("无法读取视口相机"));
    }
    camera.target = {3.3333, 3.3333, 0.0};
    camera.position = {3.3333, 3.3333, 10.0};
    camera.direction = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.orthographic = true;
    camera.orthographicHeight = 6.0;
    if (!view->setCameraState(camera)) {
        cleanup();
        return fail(QStringLiteral("无法设置属性表冒烟相机"));
    }
    waitFor(200);

    QWidget *widget = view->widget();
    if (!widget || widget->width() < 16 || widget->height() < 16) {
        cleanup();
        return fail(QStringLiteral("属性表冒烟视口尺寸无效"));
    }
    const QPoint centre(widget->width() / 2, widget->height() / 2);
    const auto clickAt = [&](const QPoint &position) {
        const QPoint global = widget->mapToGlobal(position);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(position), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position), QPointF(global),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
        QApplication::processEvents();
    };
    clickAt(centre);
    waitFor(100);

    if (table->rowCount() != 2) {
        const QString shown = title->text();
        cleanup();
        return fail(QStringLiteral("属性表行数应为 2，实际 %1（标题：%2）")
                        .arg(table->rowCount()).arg(shown));
    }
    const QStringList fields = {table->item(0, 0) ? table->item(0, 0)->text() : QString(),
                                table->item(1, 0) ? table->item(1, 0)->text() : QString()};
    const QStringList values = {table->item(0, 1) ? table->item(0, 1)->text() : QString(),
                                table->item(1, 1) ? table->item(1, 1)->text() : QString()};
    if (!fields.contains(QStringLiteral("名称")) || !fields.contains(QStringLiteral("面积")) ||
        !values.contains(QStringLiteral("烟测要素")) || !values.contains(QStringLiteral("12.5"))) {
        cleanup();
        return fail(QStringLiteral("属性表字段/值不对：%1=%2 / %3=%4")
                        .arg(fields[0], values[0], fields[1], values[1]));
    }
    if (!title->text().contains(layerName) || !title->text().contains(QStringLiteral("共 1 个要素")))
        return fail(QStringLiteral("属性表标题缺少图层名或要素总数：%1").arg(title->text()));
    if (scene->highlightedFeatureTriangleCount() < 1) {
        cleanup();
        return fail(QStringLiteral("拾取后未生成要素高亮"));
    }

    // 删掉图层再点一次：属性表应清空、高亮应撤销。
    layers->removeLayer(group);
    clickAt(centre);
    waitFor(100);
    const bool cleared = table->rowCount() == 0 && scene->highlightedFeatureTriangleCount() == 0;
    if (!cleared) {
        return fail(QStringLiteral("删除图层后属性表/高亮未清空：rows=%1 hl=%2")
                        .arg(table->rowCount())
                        .arg(scene->highlightedFeatureTriangleCount()));
    }

    std::cerr << "VIEW3D_FEATURE_TABLE_OK rows=2 highlighted=1 cleared=1" << std::endl;
    return true;
}

// 参照系矩阵必须是刚体变换：三行基向量单位正交、齐次末行 (0,0,0,1)、行列式 +1。
// 返回空串表示通过，否则是给人看的问题描述（镜像会让行列式变成 -1，模型左右颠倒）。
QString rigidFrameProblem(const View3D::ILayer::ReferenceFrame &frame)
{
    if (!frame.hasMatrix)
        return QStringLiteral("没有源→局部矩阵");
    const std::array<double, 16> &m = frame.matrix;
    if (qAbs(m[12]) > 1e-9 || qAbs(m[13]) > 1e-9 || qAbs(m[14]) > 1e-9 ||
        qAbs(m[15] - 1.0) > 1e-9)
        return QStringLiteral("齐次末行不是 (0,0,0,1)");
    for (int row = 0; row < 3; ++row) {
        const int base = row * 4;
        const double length = m[base] * m[base] + m[base + 1] * m[base + 1] +
            m[base + 2] * m[base + 2];
        if (qAbs(length - 1.0) > 1e-9)
            return QStringLiteral("第 %1 行不是单位向量").arg(row);
    }
    const auto dot = [&m](int a, int b) {
        return m[a] * m[b] + m[a + 1] * m[b + 1] + m[a + 2] * m[b + 2];
    };
    if (qAbs(dot(0, 4)) > 1e-9 || qAbs(dot(0, 8)) > 1e-9 || qAbs(dot(4, 8)) > 1e-9)
        return QStringLiteral("基向量不正交");
    const double determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (qAbs(determinant - 1.0) > 1e-9)
        return QStringLiteral("行列式不是 +1：%1").arg(determinant);
    return {};
}

// 参照系冒烟：PROJ 内核的标识/判型/批量变换、世界原点首钉，以及导入链路把源坐标
// 归到主坐标系——1° 的 KML 线段进场景必须是 11 万米级，而不是「度当米」的 1。
bool verifyCrsSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    if (!reference) {
        // 精简配置（ui_config.test.xml 只装 renderer+sceneview）里没有参照系插件，不算失败；
        // 完整配置下缺它会被后面的用例判失败。
        std::cerr << "VIEW3D_CRS_SKIPPED reason=plugin_not_loaded" << std::endl;
        return true;
    }
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *openRegistry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    if (!scene || !layers || !openRegistry)
        return fail(QStringLiteral("参照系冒烟缺少场景/图层服务或打开数据注册表"));

    if (reference->mainCrs() != QLatin1String("EPSG:4979"))
        return fail(QStringLiteral("主参照系标识异常：%1").arg(reference->mainCrs()));

    // ── 1. 参照系判型 ───────────────────────────────────────────────────────
    View3D::IGeoReference::CrsInfo geo {};
    if (!reference->crsInfo(QStringLiteral("EPSG:4326"), &geo) || !geo.geographic ||
        geo.cartesian)
        return fail(QStringLiteral("EPSG:4326 应判为经纬度系：%1/%2")
                        .arg(geo.geographic)
                        .arg(geo.cartesian));
    View3D::IGeoReference::CrsInfo projected {};
    if (!reference->crsInfo(QStringLiteral("EPSG:3857"), &projected) || !projected.cartesian ||
        projected.geographic)
        return fail(QStringLiteral("EPSG:3857 应判为直角坐标系：%1/%2")
                        .arg(projected.geographic)
                        .arg(projected.cartesian));

    // ── 2. 批量变换：Web Mercator 已知点 -> 主参照系（经度,纬度,椭球高）──────
    // 1° 经度在 EPSG:3857 里是 111319.49079327358 m；投影系的 z 按高程透传。
    std::vector<double> batch {111319.49079327358, 0.0, 12.0, 0.0, 0.0, 0.0};
    if (!reference->transformToMain(QStringLiteral("EPSG:3857"), batch.data(), 2))
        return fail(QStringLiteral("EPSG:3857 批量变换失败"));
    if (qAbs(batch[0] - 1.0) > 1e-6 || qAbs(batch[1]) > 1e-6 || qAbs(batch[2] - 12.0) > 1e-6)
        return fail(QStringLiteral("EPSG:3857 变换结果不是预期经纬度高程：%1,%2,%3")
                        .arg(batch[0], 0, 'f', 8)
                        .arg(batch[1], 0, 'f', 8)
                        .arg(batch[2], 0, 'f', 8));
    if (qAbs(batch[3]) > 1e-9 || qAbs(batch[4]) > 1e-9 || qAbs(batch[5]) > 1e-9)
        return fail(QStringLiteral("原点应变换到 0,0,0：%1,%2,%3")
                        .arg(batch[3]).arg(batch[4]).arg(batch[5]));

    // ── 3. 世界原点首钉：后来的数据源不能搬走已提交网格的位置 ─────────────────
    reference->resetOrigin();
    if (!reference->setOrigin(116.39, 39.91, 0.0))
        return fail(QStringLiteral("首个数据源无法钉住世界原点"));
    if (reference->setOrigin(120.0, 30.0, 0.0))
        return fail(QStringLiteral("世界原点被后加载的数据源搬走"));
    double pinLon = 0.0;
    double pinLat = 0.0;
    double pinHeight = 0.0;
    if (!reference->origin(&pinLon, &pinLat, &pinHeight) || qAbs(pinLon - 116.39) > 1e-12 ||
        qAbs(pinLat - 39.91) > 1e-12)
        return fail(QStringLiteral("世界原点读取异常：%1,%2,%3")
                        .arg(pinLon).arg(pinLat).arg(pinHeight));

    // ── 4. 导入时重投影：矢量自己钉住原点并把度换成米 ────────────────────────
    View3D::IDataOpener *vectorOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (opener && opener->displayName().contains(QStringLiteral("矢量"))) {
            vectorOpener = opener;
            break;
        }
    }
    if (!vectorOpener) {
        std::cerr << "VIEW3D_CRS_OK proj=1 origin_first_pin=1 import=skipped" << std::endl;
        return true;
    }
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail(QStringLiteral("无法创建参照系夹具目录"));
    const auto writeLineKml = [&directory](const QString &name, const QString &coordinates)
            -> QString {
        const QString path = QDir(directory.path()).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark>"
                   "<LineString><coordinates>"
                   + coordinates.toUtf8() +
                   "</coordinates></LineString>"
                   "</Placemark></Document></kml>\n");
        file.close();
        return path;
    };
    const QString kmlPath = writeLineKml(QStringLiteral("crs_check.kml"),
                                         QStringLiteral("0,0,0 1,0,0"));
    const QString kmlFarPath = writeLineKml(QStringLiteral("crs_check_far.kml"),
                                            QStringLiteral("10,0,0 11,0,0"));
    if (kmlPath.isEmpty() || kmlFarPath.isEmpty())
        return fail(QStringLiteral("无法写参照系 KML 夹具"));

    View3D::IDataOpener *terrainOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (opener && opener->displayName().contains(QStringLiteral("地形"))) {
            terrainOpener = opener;
            break;
        }
    }
    const QString demPath = QDir(directory.path()).filePath(QStringLiteral("crs_check_dem.tif"));
    if (terrainOpener) {
        QFile demFile(demPath);
        if (!demFile.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写参照系 GeoTIFF 夹具"));
        const QByteArray tiff = buildSmokeGeoTiff();
        if (demFile.write(tiff) != tiff.size()) {
            demFile.close();
            return fail(QStringLiteral("参照系 GeoTIFF 夹具写入不完整"));
        }
        demFile.close();
    }

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    const auto currentKeys = [scene] {
        QSet<QString> keys;
        for (const QString &key : scene->modelLayerKeys())
            keys.insert(key);
        return keys;
    };
    // 打开夹具并等它真的变成图层；导入选项对话框是模态的，无头跑要替用户点确定。
    const auto openAndWait = [&](View3D::IDataOpener *opener, const QString &path,
                                 const View3D::LayerId &targetGroup) -> QString {
        QTimer::singleShot(0, [] {
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (dialog && dialog->isModal() && dialog->isVisible() &&
                    dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                    dialog->accept();
                    return;
                }
            }
        });
        const QSet<QString> before = currentKeys();
        QString openError;
        if (!opener->openData(path, targetGroup, &openError))
            return {};
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 8000) {
            waitFor(50);
            for (const QString &key : scene->modelLayerKeys()) {
                if (!before.contains(key))
                    return key;
            }
        }
        return {};
    };
    // 顶点包围盒：{水平对角线(X/Z), 三维对角线, 中心点}。没重投影时水平分量是「度」
    // 量级（1e-4~1），重投影后是米，两者差五个数量级，判据可以放心放宽。
    const auto meshBox = [scene](const QString &key) -> std::array<double, 5> {
        const auto mesh = scene->modelLayerMesh(key);
        if (!mesh || mesh->vertices.empty())
            return {};
        std::array<double, 3> minimum {mesh->vertices.front().x, mesh->vertices.front().y,
                                       mesh->vertices.front().z};
        std::array<double, 3> maximum = minimum;
        for (const View3D::MeshVertexDto &vertex : mesh->vertices) {
            minimum[0] = std::min(minimum[0], vertex.x);
            maximum[0] = std::max(maximum[0], vertex.x);
            minimum[1] = std::min(minimum[1], vertex.y);
            maximum[1] = std::max(maximum[1], vertex.y);
            minimum[2] = std::min(minimum[2], vertex.z);
            maximum[2] = std::max(maximum[2], vertex.z);
        }
        const double dx = maximum[0] - minimum[0];
        const double dy = maximum[1] - minimum[1];
        const double dz = maximum[2] - minimum[2];
        std::array<double, 5> result {};
        result[0] = std::sqrt(dx * dx + dz * dz);
        result[1] = std::sqrt(dx * dx + dy * dy + dz * dz);
        result[2] = (maximum[0] + minimum[0]) * 0.5;
        result[3] = (maximum[1] + minimum[1]) * 0.5;
        result[4] = (maximum[2] + minimum[2]) * 0.5;
        return result;
    };
    const auto sourceCrsOf = [&](const QString &key) -> QString {
        return layers->layerSourceCrs(layers->layerByRenderKey(key));
    };

    const View3D::LayerId group = layers->addGroup(QStringLiteral("参照系冒烟组"));
    if (group.isNull())
        return fail(QStringLiteral("无法创建参照系冒烟组"));

    // ── 4a. 矢量：导入自己钉住原点，1° 线段必须是 111319 米 ──────────────────
    reference->resetOrigin();
    const QString kmlKey = openAndWait(vectorOpener, kmlPath, group);
    if (kmlKey.isEmpty()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("参照系 KML 夹具没有生成图层"));
    }
    double originLon = 0.0;
    double originLat = 0.0;
    double originHeight = 0.0;
    if (!reference->origin(&originLon, &originLat, &originHeight) ||
        qAbs(originLon - 0.5) > 1e-6 || qAbs(originLat) > 1e-6)
        return fail(QStringLiteral("矢量导入没有把世界原点钉在数据中心：%1,%2")
                        .arg(originLon, 0, 'f', 8)
                        .arg(originLat, 0, 'f', 8));
    const std::array<double, 5> kmlBox = meshBox(kmlKey);
    const double kmlSpan = kmlBox[1];
    // 赤道 1° 经度 = 111319 m；仍是「度当米」的话这里只有 1 米量级。
    if (kmlSpan < 1.085e5 || kmlSpan > 1.14e5)
        return fail(QStringLiteral("矢量未按主参照系重投影，几何跨度=%1").arg(kmlSpan));
    const QString kmlSourceCrs = sourceCrsOf(kmlKey);
    if (kmlSourceCrs.isEmpty())
        return fail(QStringLiteral("矢量图层没有记录源参照系"));
    if (!kmlSourceCrs.contains(QStringLiteral("4326"), Qt::CaseInsensitive) &&
        !kmlSourceCrs.contains(QStringLiteral("WGS"), Qt::CaseInsensitive))
        return fail(QStringLiteral("矢量图层源参照系标识异常：%1").arg(kmlSourceCrs));
    // 经 PROJ 归一的图层：给世界原点经纬度，但不该伪造仿射矩阵（源→局部非线性）。
    const View3D::ILayer::ReferenceFrame vectorFrame =
        layers->layerReferenceFrame(layers->layerByRenderKey(kmlKey));
    if (vectorFrame.hasMatrix)
        return fail(QStringLiteral("矢量图层不应带仿射矩阵：源→局部经过椭球投影"));
    if (vectorFrame.position.isEmpty())
        return fail(QStringLiteral("矢量图层缺少世界原点经纬度"));

    // ── 4b. 多源叠加：第二个矢量不得搬走原点，两者间距必须是真实公里数 ─────────
    const QString kmlFarKey = openAndWait(vectorOpener, kmlFarPath, group);
    if (kmlFarKey.isEmpty()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("第二份参照系 KML 夹具没有生成图层"));
    }
    double reusedLon = 0.0;
    double reusedLat = 0.0;
    double reusedHeight = 0.0;
    if (!reference->origin(&reusedLon, &reusedLat, &reusedHeight) ||
        qAbs(reusedLon - originLon) > 1e-9 || qAbs(reusedLat - originLat) > 1e-9) {
        layers->removeLayer(group);
        return fail(QStringLiteral("后加载的矢量搬走了世界原点：%1,%2 ← %3,%4")
                        .arg(reusedLon, 0, 'f', 8)
                        .arg(reusedLat, 0, 'f', 8)
                        .arg(originLon, 0, 'f', 8)
                        .arg(originLat, 0, 'f', 8));
    }
    const std::array<double, 5> farBox = meshBox(kmlFarKey);
    const double gap = std::sqrt((farBox[2] - kmlBox[2]) * (farBox[2] - kmlBox[2]) +
                                 (farBox[3] - kmlBox[3]) * (farBox[3] - kmlBox[3]) +
                                 (farBox[4] - kmlBox[4]) * (farBox[4] - kmlBox[4]));
    // 两份数据中心相距 10° 经度：切平面里 ≈ 1.11e6 m；按度当米只有 ≈10。
    if (gap < 1.05e6 || gap > 1.18e6)
        return fail(QStringLiteral("多源叠加没有共用同一个原点，中心间距=%1").arg(gap));

    // ── 4c. 栅格：约 88 m 见方的 GeoTIFF 进场景必须是米，且继续复用原点 ────────
    double rasterSpan = 0.0;
    QString rasterSourceCrs;
    if (terrainOpener) {
        const QString demKey = openAndWait(terrainOpener, demPath, group);
        if (demKey.isEmpty()) {
            layers->removeLayer(group);
            return fail(QStringLiteral("参照系 GeoTIFF 夹具没有生成图层"));
        }
        rasterSourceCrs = sourceCrsOf(demKey);
        // 裸字节写的 GeoKeyDirectory（含 .prj 边车）GDAL 读不出 SRS，只有 ModelTiePoint
        // 生效，因此这条路只能退化成「按源坐标显示」并出告警。真成果的 GeoTIFF 有 SRS，
        // 会走上面的跨度断言；这里显式记录缺口，不当成通过。
        if (rasterSourceCrs.isEmpty()) {
            rasterSpan = meshBox(demKey)[0];
            std::cerr << "VIEW3D_CRS_PARTIAL reason=raster_fixture_without_crs" << std::endl;
        } else {
            rasterSpan = meshBox(demKey)[0];
            // 夹具 8px × 0.0001°/px ≈ 94 m 水平对角线；没重投影时它是 1e-3 量级。
            if (rasterSpan < 60.0 || rasterSpan > 160.0) {
                layers->removeLayer(group);
                return fail(QStringLiteral("栅格未按主参照系重投影，水平跨度=%1")
                                .arg(rasterSpan));
            }
            if (!rasterSourceCrs.contains(QStringLiteral("4326"), Qt::CaseInsensitive) &&
                !rasterSourceCrs.contains(QStringLiteral("WGS"), Qt::CaseInsensitive)) {
                layers->removeLayer(group);
                return fail(QStringLiteral("栅格图层源参照系标识异常：%1")
                                .arg(rasterSourceCrs));
            }
        }
        double rasterLon = 0.0;
        double rasterLat = 0.0;
        double rasterHeight = 0.0;
        if (!reference->origin(&rasterLon, &rasterLat, &rasterHeight) ||
            qAbs(rasterLon - originLon) > 1e-9 || qAbs(rasterLat - originLat) > 1e-9) {
            layers->removeLayer(group);
            return fail(QStringLiteral("栅格导入搬走了矢量钉住的世界原点：%1,%2 ← %3,%4")
                            .arg(rasterLon, 0, 'f', 8)
                            .arg(rasterLat, 0, 'f', 8)
                            .arg(originLon, 0, 'f', 8)
                            .arg(originLat, 0, 'f', 8));
        }
    }

    layers->removeLayer(group);
    reference->resetOrigin();
    std::cerr << "VIEW3D_CRS_OK proj=1 origin_first_pin=1 import=1 span=" << kmlSpan
              << " gap=" << gap << " raster=" << rasterSpan
              << " sourceCrs=" << kmlSourceCrs.toStdString() << "/"
              << rasterSourceCrs.toStdString() << std::endl;
    return true;
}

// 动态投影冒烟：切换显示参照系后，带源地理坐标缓存的图层必须「原地重放」——图层
// 身份不变、不新增图层、坐标与新投影自洽，切回经纬度显示还能回到原来的位置。
// 另外断言投影微分（收敛角/比例因子）的数值，瓦片类的刚体跟随就是用它摆的。
bool verifyDynamicProjectionSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    if (!reference) {
        std::cerr << "VIEW3D_CRS_PROJ_SKIPPED reason=plugin_not_loaded" << std::endl;
        return true;
    }
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *openRegistry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    if (!scene || !layers || !openRegistry)
        return fail(QStringLiteral("动态投影冒烟缺少场景/图层服务或打开数据注册表"));
    View3D::IDataOpener *vectorOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (opener && opener->displayName().contains(QStringLiteral("矢量"))) {
            vectorOpener = opener;
            break;
        }
    }
    if (!vectorOpener) {
        std::cerr << "VIEW3D_CRS_PROJ_SKIPPED reason=no_vector_opener" << std::endl;
        return true;
    }

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };

    // ── 0. 默认不启用：开关关着时不能改变任何摆放 ────────────────────────────
    if (reference->dynamicProjectionEnabled())
        return fail(QStringLiteral("动态投影默认应当关闭，实测已开启"));
    if (reference->displayCrs() != QLatin1String("EPSG:4979"))
        return fail(QStringLiteral("关闭状态下生效的显示参照系应是主参照系：%1")
                        .arg(reference->displayCrs()));
    // 倾斜摄影那类「按源投影网格轴摆放」的成果，关闭时不得被收敛角旋转或缩放。
    View3D::IGeoReference::PlanePlacement offPlacement {};
    if (!reference->planePlacement(QStringLiteral("EPSG:4547"), 114.1945, 22.5977, 0.0,
                                   &offPlacement))
        return fail(QStringLiteral("关闭状态下求跟随参数失败：%1").arg(reference->lastError()));
    if (offPlacement.yawDegrees != 0.0 || offPlacement.horizontalScale != 1.0 ||
        offPlacement.originLocal[0] != 0.0 || offPlacement.originLocal[2] != 0.0) {
        return fail(QStringLiteral("关闭状态下跟随必须是恒等：%1°/%2/%3,%4")
                        .arg(offPlacement.yawDegrees, 0, 'f', 6)
                        .arg(offPlacement.horizontalScale, 0, 'f', 8)
                        .arg(offPlacement.originLocal[0])
                        .arg(offPlacement.originLocal[2]));
    }

    // ── 1. 平面微分数值 ─────────────────────────────────────────────────────
    // EPSG:3857 是球面墨卡托：30°N 处「地面米 → 网格米」的比例 ≈ 1/cos30°=1.1547，
    // 网格北与真北同向（收敛角 0）。
    View3D::IGeoReference::PlaneFrame mercator {};
    if (!reference->planeFrameAt(QStringLiteral("EPSG:3857"), 117.0, 30.0, 0.0, &mercator))
        return fail(QStringLiteral("EPSG:3857 的平面微分求不出来：%1").arg(reference->lastError()));
    if (std::abs(mercator.convergenceDeg) > 0.02 || mercator.scaleFactor < 1.13 ||
        mercator.scaleFactor > 1.19) {
        return fail(QStringLiteral("EPSG:3857 微分异常：收敛角 %1，比例因子 %2")
                        .arg(mercator.convergenceDeg, 0, 'f', 4)
                        .arg(mercator.scaleFactor, 0, 'f', 5));
    }
    View3D::IGeoReference::PlaneFrame geographic {};
    if (!reference->planeFrameAt(QStringLiteral("EPSG:4979"), 117.0, 30.0, 0.0, &geographic) ||
        std::abs(geographic.convergenceDeg) > 1e-9 ||
        std::abs(geographic.scaleFactor - 1.0) > 1e-9)
        return fail(QStringLiteral("经纬度显示的平面微分应当是单位阵"));
    // UTM 要靠 PROJ。EPSG:32650（50 带）的中央经线是 117°E：带上的点收敛角为 0，
    // 往东 3°（120°E）、30°N 处的收敛角约 Δλ·sinφ = 3°×0.5 = 1.5°，比例因子约
    // k0·(1 + (Δλ·cosφ)²/2) ≈ 0.9996×1.001 = 1.0006。
    double utmConvergence = 0.0;
    double utmCentralConvergence = 0.0;
    View3D::IGeoReference::CrsInfo utmInfo {};
    if (reference->crsInfo(QStringLiteral("EPSG:32650"), &utmInfo)) {
        View3D::IGeoReference::PlaneFrame central {};
        if (!reference->planeFrameAt(QStringLiteral("EPSG:32650"), 117.0, 30.0, 0.0, &central))
            return fail(QStringLiteral("EPSG:32650 中央经线上的平面微分求不出来"));
        utmCentralConvergence = central.convergenceDeg;
        if (std::abs(central.convergenceDeg) > 0.02)
            return fail(QStringLiteral("UTM 50N 在中央经线 117°E 的收敛角应为 0，实为 %1")
                            .arg(central.convergenceDeg, 0, 'f', 4));
        if (std::abs(central.scaleFactor - 0.9996) > 0.0005)
            return fail(QStringLiteral("UTM 50N 中央经线上的比例因子应为 0.9996，实为 %1")
                            .arg(central.scaleFactor, 0, 'f', 6));
        View3D::IGeoReference::PlaneFrame utm {};
        if (!reference->planeFrameAt(QStringLiteral("EPSG:32650"), 120.0, 30.0, 0.0, &utm))
            return fail(QStringLiteral("EPSG:32650 的平面微分求不出来"));
        utmConvergence = utm.convergenceDeg;
        if (utmConvergence < 1.2 || utmConvergence > 1.8)
            return fail(QStringLiteral("UTM 50N 在 120°E·30°N 的收敛角应约 1.5°，实为 %1")
                            .arg(utmConvergence, 0, 'f', 3));
        if (utm.scaleFactor < 0.999 || utm.scaleFactor > 1.004)
            return fail(QStringLiteral("UTM 50N 在 120°E 的比例因子异常：%1")
                            .arg(utm.scaleFactor, 0, 'f', 6));
    } else {
        std::cerr << "VIEW3D_CRS_PROJ_PARTIAL reason=proj_absent_for_utm" << std::endl;
    }

    // ── 2. 导入矢量，确认源地理坐标缓存落在网格上 ────────────────────────────
    // 缓存只在开关打开时才建，所以这一步先启用动态投影（用例末尾再关回去）。
    reference->setDynamicProjectionEnabled(true);
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail(QStringLiteral("无法创建动态投影夹具目录"));
    // 39.91°N 上 0.01° 经度：真北切平面里约 855 m，墨卡托网格里约 1113 m，
    // 差 250 m 以上，重放与否一眼就能判出来。
    const QString kmlPath = QDir(directory.path()).filePath(QStringLiteral("proj_check.kml"));
    {
        QFile file(kmlPath);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写动态投影夹具"));
        file.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark>"
                   "<LineString><coordinates>"
                   "116.39,39.91,0 116.40,39.91,0"
                   "</coordinates></LineString></Placemark></Document></kml>\n");
        file.close();
    }
    reference->resetOrigin();
    // 导入选项对话框是模态的：无头跑要在下一轮事件循环替用户点确定。
    QTimer::singleShot(0, [] {
        const auto widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isModal() && dialog->isVisible() &&
                dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                dialog->accept();
                return;
            }
        }
    });
    const View3D::LayerId group = layers->addGroup(QStringLiteral("动态投影组"));
    if (group.isNull())
        return fail(QStringLiteral("无法创建动态投影组"));
    QSet<QString> before;
    for (const QString &key : scene->modelLayerKeys())
        before.insert(key);
    QString openError;
    if (!vectorOpener->openData(kmlPath, group, &openError)) {
        layers->removeLayer(group);
        return fail(QStringLiteral("动态投影夹具打开失败：%1").arg(openError));
    }
    QString layerKey;
    for (int attempt = 0; attempt < 160 && layerKey.isEmpty(); ++attempt) {
        waitFor(50);
        for (const QString &key : scene->modelLayerKeys()) {
            if (!before.contains(key)) {
                layerKey = key;
                break;
            }
        }
    }
    if (layerKey.isEmpty()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("动态投影夹具没有生成图层"));
    }
    const auto meshOf = [scene, &layerKey] { return scene->modelLayerMesh(layerKey); };
    const std::shared_ptr<const View3D::ModelMeshDto> imported = meshOf();
    if (!imported || imported->geoVertices.empty() ||
        imported->geoVertices.size() != imported->vertices.size() * 3) {
        layers->removeLayer(group);
        return fail(QStringLiteral("矢量网格没有缓存源地理坐标（geoVertices=%1）")
                        .arg(imported ? imported->geoVertices.size() : -1));
    }
    if (imported->geoCrs != QLatin1String("EPSG:4979")) {
        layers->removeLayer(group);
        return fail(QStringLiteral("源坐标缓存的参照系标识异常：%1").arg(imported->geoCrs));
    }
    const View3D::LayerId layer = layers->layerByRenderKey(layerKey);
    if (layer.isNull()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("动态投影图层反查失败"));
    }
    const View3D::MeshVertexDto firstBefore = imported->vertices.front();
    const std::array<double, 3> firstGeo {imported->geoVertices[0], imported->geoVertices[1],
                                          imported->geoVertices[2]};
    const qsizetype layerCount = scene->modelLayerKeys().size();

    // ── 3. 切到 Web Mercator：应原地重放，而不是新增/重建图层 ─────────────────
    if (!reference->setDisplayCrs(QStringLiteral("EPSG:3857"))) {
        layers->removeLayer(group);
        return fail(QStringLiteral("切换 EPSG:3857 失败：%1").arg(reference->lastError()));
    }
    std::shared_ptr<const View3D::ModelMeshDto> reprojected;
    bool replayed = false;
    for (int attempt = 0; attempt < 200 && !replayed; ++attempt) {
        waitFor(50);
        const auto current = meshOf();
        if (current && current->vertices.size() == imported->vertices.size() &&
            std::abs(current->vertices.front().x - firstBefore.x) > 1.0) {
            reprojected = current;
            replayed = true;
        }
    }
    if (!replayed) {
        reference->setDisplayCrs(QStringLiteral("EPSG:4979"));
        layers->removeLayer(group);
        return fail(QStringLiteral("切换投影后矢量图层没有原地重放"));
    }
    if (scene->modelLayerKeys().size() != layerCount) {
        reference->setDisplayCrs(QStringLiteral("EPSG:4979"));
        layers->removeLayer(group);
        return fail(QStringLiteral("重放换了图层而不是替换几何"));
    }
    if (layers->layerSourceCrs(layer).isEmpty()) {
        reference->setDisplayCrs(QStringLiteral("EPSG:4979"));
        layers->removeLayer(group);
        return fail(QStringLiteral("重放后图层身份变了：源参照系读不到"));
    }
    // 自洽：新局部坐标反算回经纬度，必须还是缓存里那份源坐标（1 cm 级）。
    double backLon = 0.0;
    double backLat = 0.0;
    double backHeight = 0.0;
    const std::array<double, 3> firstAfter {
        reprojected->vertices.front().x, reprojected->vertices.front().y,
        reprojected->vertices.front().z};
    if (!reference->localToLonLatHeight(firstAfter, &backLon, &backLat, &backHeight) ||
        std::abs(backLon - firstGeo[0]) > 1e-7 || std::abs(backLat - firstGeo[1]) > 1e-7) {
        reference->setDisplayCrs(QStringLiteral("EPSG:4979"));
        layers->removeLayer(group);
        return fail(QStringLiteral("重放后的坐标与源缓存不一致：%1,%2 ← %3,%4")
                        .arg(backLon, 0, 'f', 8)
                        .arg(backLat, 0, 'f', 8)
                        .arg(firstGeo[0], 0, 'f', 8)
                        .arg(firstGeo[1], 0, 'f', 8));
    }

    // ── 4. 切回经纬度显示：位置要回到原样（可逆） ────────────────────────────
    if (!reference->setDisplayCrs(QStringLiteral("EPSG:4979"))) {
        layers->removeLayer(group);
        return fail(QStringLiteral("切回 EPSG:4979 失败：%1").arg(reference->lastError()));
    }
    bool restored = false;
    for (int attempt = 0; attempt < 200 && !restored; ++attempt) {
        waitFor(50);
        const auto current = meshOf();
        if (current && std::abs(current->vertices.front().x - firstBefore.x) < 1e-3 &&
            std::abs(current->vertices.front().y - firstBefore.y) < 1e-3 &&
            std::abs(current->vertices.front().z - firstBefore.z) < 1e-3)
            restored = true;
    }
    if (!restored) {
        layers->removeLayer(group);
        return fail(QStringLiteral("切回经纬度显示后顶点没有回到原位"));
    }

    // ── 5. 刚体跟随参数：瓦片类数据按它摆（源是真北 ENU，目标 3857） ──────────
    double originLon = 0.0;
    double originLat = 0.0;
    double originHeight = 0.0;
    if (!reference->origin(&originLon, &originLat, &originHeight)) {
        layers->removeLayer(group);
        return fail(QStringLiteral("矢量导入没有钉住世界原点"));
    }
    View3D::IGeoReference::PlanePlacement placement {};
    if (!reference->planePlacement(QStringLiteral("EPSG:4979"), originLon, originLat,
                                   originHeight, &placement)) {
        layers->removeLayer(group);
        return fail(QStringLiteral("刚体跟随参数求不出来：%1").arg(reference->lastError()));
    }
    if (std::abs(placement.yawDegrees) > 0.02 || std::abs(placement.horizontalScale - 1.0) > 1e-6)
        return fail(QStringLiteral("经纬度显示下的跟随应是恒等：%1°/%2")
                        .arg(placement.yawDegrees, 0, 'f', 6)
                        .arg(placement.horizontalScale, 0, 'f', 6));

    layers->removeLayer(group);
    reference->resetOrigin();
    // 回到默认状态：后面的倾斜摄影/瓦片用例是按「未启用动态投影」的摆放写断言的。
    reference->setDynamicProjectionEnabled(false);
    std::cerr << "VIEW3D_CRS_PROJ_OK merc_scale=" << mercator.scaleFactor
              << " merc_conv=" << mercator.convergenceDeg
              << " utm_cm_conv=" << utmCentralConvergence << " utm_conv=" << utmConvergence
              << " replay=1 restore=1 layer_stable=1 follow_yaw=" << placement.yawDegrees
              << " follow_scale=" << placement.horizontalScale << std::endl;
    return true;
}

// 视口底部坐标读数冒烟：每个可见视口各一条；悬停拾取按监听者广播；二维正俯视下
// 视线必然与地面相交，用来确定性地验证「没碰到模型也给地面交点」这条路径；钉住
// 世界原点后读数要带经纬度，且与 IGeoReference 的反算一致。
bool verifyViewportCoordinateReadout(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!layout || layout->views().isEmpty()) {
        std::cerr << "VIEW3D_VP_COORD_SKIPPED reason=no_viewport" << std::endl;
        return true;
    }
    const auto labelFor = [](View3D::IRenderSurface *view) -> QLabel * {
        QWidget *widget = view ? view->widget() : nullptr;
        if (!widget || !widget->parentWidget())
            return nullptr;
        return widget->parentWidget()->findChild<QLabel *>(
            QStringLiteral("viewport.coordinateReadout"));
    };
    const int shown = qMin(layout->views().size(), layout->layoutViewportCount());
    for (int index = 0; index < shown; ++index) {
        QLabel *readout = labelFor(layout->views().at(index));
        if (!readout)
            return fail(QStringLiteral("第 %1 个视口缺少底部坐标读数").arg(index + 1));
        if (!readout->text().startsWith(QStringLiteral("坐标")))
            return fail(QStringLiteral("读数条初始不是占位符：%1").arg(readout->text()));
    }

    auto *reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    auto *primary = layout->primaryView();
    QWidget *widget = primary ? primary->widget() : nullptr;
    if (!widget || widget->width() <= 0 || widget->height() <= 0)
        return fail(QStringLiteral("主视口还没有可用尺寸"));

    int hoverHits = 0;
    View3D::ViewportPickResult last;
    const View3D::PickListenerId token = primary->addHoverPickListener(
        [&hoverHits, &last](const View3D::ViewportPickResult &result) {
            ++hoverHits;
            last = result;
        });
    const auto hoverCenter = [&] {
        const QPoint position(widget->width() / 2, widget->height() / 2);
        const QPoint global = widget->mapToGlobal(position);
        QMouseEvent move(QEvent::MouseMove, QPointF(position), QPointF(global), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        // 悬停拾取按 80 ms 节流，这里连发两次并等过节流窗口。
        QApplication::sendEvent(widget, &move);
        QApplication::processEvents();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 140) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        QApplication::sendEvent(widget, &move);
        QApplication::processEvents();
    };

    const bool wasPlanar = primary->planarMode();
    primary->setPlanarMode(true);
    QApplication::processEvents();
    const int beforeCount = hoverHits;
    hoverCenter();
    if (hoverHits <= beforeCount) {
        primary->setPlanarMode(wasPlanar);
        primary->removeHoverPickListener(token);
        return fail(QStringLiteral("悬停没有广播拾取结果（次数 %1）").arg(hoverHits));
    }
    if (!last.hasWorldPosition) {
        primary->setPlanarMode(wasPlanar);
        primary->removeHoverPickListener(token);
        return fail(QStringLiteral("二维正俯视下没有拿到地面交点"));
    }
    if (std::abs(last.worldPosition[1]) > 1e-3) {
        primary->setPlanarMode(wasPlanar);
        primary->removeHoverPickListener(token);
        return fail(QStringLiteral("地面交点的高程不是 0：%1").arg(last.worldPosition[1]));
    }
    QLabel *primaryReadout = labelFor(primary);
    if (!primaryReadout)
        return fail(QStringLiteral("主视口读数条丢了"));
    const QString shownText = primaryReadout->text();
    if (!shownText.startsWith(QStringLiteral("X ")) || !shownText.contains(QStringLiteral("Y ")) ||
        !shownText.contains(QStringLiteral("（地面）"))) {
        primary->setPlanarMode(wasPlanar);
        primary->removeHoverPickListener(token);
        return fail(QStringLiteral("读数条没显示地面交点：%1").arg(shownText));
    }

    // 光标移出视口：读数收回占位符，不能把上一个坐标留在屏上。
    {
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(widget, &leave);
        QApplication::processEvents();
        if (!primaryReadout->text().startsWith(QStringLiteral("坐标"))) {
            primary->setPlanarMode(wasPlanar);
            primary->removeHoverPickListener(token);
            return fail(QStringLiteral("鼠标离开后读数没清空：%1").arg(primaryReadout->text()));
        }
    }

    // 经纬度：钉住世界原点后再悬停，读数要带度，且与 crs 服务自己的反算一致。
    if (reference) {
        reference->resetOrigin();
        if (!reference->setOrigin(116.39, 39.91, 0.0)) {
            primary->setPlanarMode(wasPlanar);
            primary->removeHoverPickListener(token);
            return fail(QStringLiteral("坐标读数用例钉不住世界原点"));
        }
        const int geoBefore = hoverHits;
        hoverCenter();
        const QString geoText = primaryReadout->text();
        double lonDeg = 0.0;
        double latDeg = 0.0;
        double heightMeters = 0.0;
        const bool inverted =
            reference->localToLonLatHeight(last.worldPosition, &lonDeg, &latDeg, &heightMeters);
        const QString expectedLon = QString::number(lonDeg, 'f', 6);
        if (hoverHits <= geoBefore || !inverted || !geoText.contains(expectedLon) ||
            !geoText.contains(QStringLiteral("°"))) {
            primary->setPlanarMode(wasPlanar);
            primary->removeHoverPickListener(token);
            reference->resetOrigin();
            return fail(QStringLiteral("读数条的经纬度与反算不一致：%1（期望含 %2，反算=%3）")
                            .arg(geoText, expectedLon, inverted ? QStringLiteral("ok")
                                                               : QStringLiteral("失败")));
        }
        reference->resetOrigin();
        std::cerr << "VIEW3D_VP_COORD_OK views=" << shown << " hover=" << hoverHits
                  << " lon=" << expectedLon.toStdString() << " text=" << shownText.toStdString()
                  << std::endl;
    } else {
        std::cerr << "VIEW3D_VP_COORD_PARTIAL reason=no_crs_service text="
                  << shownText.toStdString() << std::endl;
    }
    primary->setPlanarMode(wasPlanar);
    primary->removeHoverPickListener(token);
    return true;
}

// 参照系管理冒烟：直接驱动「参照系管理」面板的控件（无头，不截图），验证
// 新建条目、拒绝无法解析的定义、设为显示参照系后投影真生效、库文件写盘内容、
// Ribbon 下拉联动，以及删除流程。库文件由 ctest 的 VIEW3D_CRS_LIBRARY 指到 bin 下
// 的测试文件，不碰部署目录里用户自己的库。
bool verifyCrsLibrarySmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *reference = container
        ? container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    if (!reference) {
        std::cerr << "VIEW3D_CRS_LIB_SKIPPED reason=plugin_not_loaded" << std::endl;
        return true;
    }
    QWidget *window = application.getMainWindow();
    auto *dock = window ? window->findChild<QWidget *>(QStringLiteral("dw.crs_manager")) : nullptr;
    if (!dock) {
        // 精简配置（ui_config.test.xml）里没有 crs 插件的界面，不算失败。
        std::cerr << "VIEW3D_CRS_LIB_SKIPPED reason=no_dock" << std::endl;
        return true;
    }
    // 没配测试库文件（VIEW3D_CRS_LIBRARY）的用例一律跳过：本用例会写文件，
    // 默认的 bin/crs_library.json 是用户真在用的那份，不能被冒烟数据污染。
    const QString libraryPath = qEnvironmentVariable("VIEW3D_CRS_LIBRARY");
    if (libraryPath.trimmed().isEmpty()) {
        std::cerr << "VIEW3D_CRS_LIB_SKIPPED reason=library_path_unset" << std::endl;
        return true;
    }
    auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("crs.manager.tree"));
    auto *list = dock->findChild<QListWidget *>(QStringLiteral("crs.manager.list"));
    auto *search = dock->findChild<QLineEdit *>(QStringLiteral("crs.manager.search"));
    auto *details = dock->findChild<QTextBrowser *>(QStringLiteral("crs.manager.details"));
    auto *status = dock->findChild<QLabel *>(QStringLiteral("crs.manager.status"));
    auto *newButton = dock->findChild<QPushButton *>(QStringLiteral("crs.manager.new"));
    auto *copyProj = dock->findChild<QPushButton *>(QStringLiteral("crs.manager.copy_proj"));
    // 收藏/编辑/删除/存进自定义 挂在面板的右键菜单里；动作对象有 objectName，
    // 无头用例直接 trigger，不必模拟鼠标。
    auto *favoriteAction = dock->findChild<QAction *>(QStringLiteral("crs.action.favorite"));
    auto *editAction = dock->findChild<QAction *>(QStringLiteral("crs.action.edit"));
    auto *promoteAction = dock->findChild<QAction *>(QStringLiteral("crs.action.promote"));
    auto *deleteAction = dock->findChild<QAction *>(QStringLiteral("crs.action.delete"));
    if (!tree || !list || !search || !details || !status || !newButton || !copyProj ||
        !favoriteAction || !editAction || !promoteAction || !deleteAction)
        return fail(QStringLiteral("参照系管理面板的控件不全，无法验证"));
    if (!newButton->isEnabled())
        return fail(QStringLiteral("参照系管理面板没拿到库依赖（新建按钮是灰的）"));
    auto *library = container
        ? container->objectAs<View3D::ICrsLibrary>(View3D::ServiceIds::CrsLibrary)
        : nullptr;
    auto *browser = container
        ? container->objectAs<View3D::ICrsBrowser>(View3D::ServiceIds::CrsBrowser)
        : nullptr;
    if (!library)
        return fail(QStringLiteral("参照系列库服务取不到，验不了导入导出与编辑"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    // 列表行的身份存在 item data 里，角色号和 CrsManagerDock.cpp 的常量一一对应：
    // UserRole 是来源（fav/custom/builtin），+1 是交给后端的定义文本，+2 是库条目 id。
    const auto rowDefinition = [list](int row) {
        if (row < 0 || row >= list->count())
            return QString();
        return list->item(row)->data(Qt::UserRole + 1).toString();
    };
    const auto rowLibraryId = [list](int row) {
        if (row < 0 || row >= list->count())
            return QString();
        return list->item(row)->data(Qt::UserRole + 2).toString();
    };
    const auto findRow = [&](const QString &definition) {
        for (int index = 0; index < list->count(); ++index) {
            if (rowDefinition(index) == definition)
                return index;
        }
        return -1;
    };
    // 左树按显示名前缀选分支（常用 / 自定义 / EPSG 内置 / 具体分组）。
    const auto selectTreeNode = [&](const QString &prefix) {
        const auto walk = [&](const auto &self, QTreeWidgetItem *node) -> bool {
            if (!node)
                return false;
            if (node->text(0).startsWith(prefix)) {
                tree->setCurrentItem(node);
                return true;
            }
            for (int index = 0; index < node->childCount(); ++index) {
                if (self(self, node->child(index)))
                    return true;
            }
            return false;
        };
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            if (walk(walk, tree->topLevelItem(index))) {
                waitFor(150);
                return true;
            }
        }
        return false;
    };
    const QString customName = QStringLiteral("冒烟自定义 CM111 带");
    const QString customDefinition = QStringLiteral(
        "+proj=tmerc +lat_0=0 +lon_0=111 +k_0=0.9996 +x_0=500000 +y_0=0 "
        "+ellps=GRS80 +units=m +no_defs");
    const auto acceptDeleteDialog = [] {
        QTimer::singleShot(0, [] {
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->windowTitle().contains(QStringLiteral("删除参照系")))
                    continue;
                // 按「用户点了 Yes」这条路走：直接 done(code) 时 QMessageBox 的返回值
                // 不一定映射成 StandardButton，确认框会被当成取消。
                if (auto *yes = box->button(QMessageBox::Yes))
                    yes->click();
                else
                    box->done(QMessageBox::Yes);
                return;
            }
        });
    };
    // 新建/编辑是模态对话框：在下一轮事件循环里替用户填写并点确定，顺便把
    // 「确定按钮当时是否可用」带回来——非法定义必须点不了。
    bool editorSawEnabledOk = false;
    const auto driveEditor = [&](const QString &name, const QString &definition, bool accept) {
        editorSawEnabledOk = false;
        QTimer::singleShot(0, [name, definition, accept, &editorSawEnabledOk] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || dialog->objectName() != QLatin1String("crs.editor.dialog") ||
                    !dialog->isVisible())
                    continue;
                auto *nameInput = dialog->findChild<QLineEdit *>(QStringLiteral("crs.editor.name"));
                auto *definitionInput = dialog->findChild<QPlainTextEdit *>(
                    QStringLiteral("crs.editor.definition"));
                auto *ok = dialog->findChild<QPushButton *>(QStringLiteral("crs.editor.ok"));
                if (nameInput && !name.isEmpty())
                    nameInput->setText(name);
                if (definitionInput && !definition.isEmpty())
                    definitionInput->setPlainText(definition);
                editorSawEnabledOk = ok && ok->isEnabled();
                if (accept && ok && ok->isEnabled())
                    ok->click();
                else
                    dialog->reject();
                return;
            }
        });
    };
    // 「选择参照系…」弹的是同一个浏览控件的选择器：替用户输入搜索词、选第一行、确定。
    bool selectorSawOk = false;
    const auto driveSelector = [&](const QString &query, bool accept) {
        selectorSawOk = false;
        QTimer::singleShot(0, [query, accept, &selectorSawOk] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || dialog->objectName() != QLatin1String("crs.select.dialog") ||
                    !dialog->isVisible())
                    continue;
                auto *search = dialog->findChild<QLineEdit *>(QStringLiteral("crs.manager.search"));
                auto *rowList = dialog->findChild<QListWidget *>(QStringLiteral("crs.manager.list"));
                auto *ok = dialog->findChild<QPushButton *>(QStringLiteral("crs.select.ok"));
                if (search && !query.isEmpty())
                    search->setText(query);
                if (rowList && rowList->count() > 0)
                    rowList->setCurrentRow(0);
                selectorSawOk = ok && ok->isEnabled();
                if (accept && ok && ok->isEnabled())
                    ok->click();
                else
                    dialog->reject();
                return;
            }
        });
    };
    if (!selectTreeNode(QStringLiteral("自定义")))
        return fail(QStringLiteral("左树里没有「自定义」分支"));
    // 上一轮如果跑挂在中间，库文件里会留下冒烟条目，而这里的断言都是「行数增量」，
    // 所以先把残留清干净，用例才能反复跑。
    const QStringList smokePrefixes { customName, QStringLiteral("冒烟非法条目") };
    for (int guard = 0; guard < 8; ++guard) {
        int stale = -1;
        for (int index = 0; index < list->count(); ++index) {
            const QString text = list->item(index)->text();
            for (const QString &prefix : smokePrefixes) {
                if (text.startsWith(prefix)) {
                    stale = index;
                    break;
                }
            }
            if (stale >= 0)
                break;
        }
        if (stale < 0)
            break;
        list->setCurrentRow(stale);
        waitFor(40);
        acceptDeleteDialog();
        deleteAction->trigger();
        waitFor(120);
        for (int index = 0; index < list->count(); ++index) {
            const QString text = list->item(index)->text();
            for (const QString &prefix : smokePrefixes) {
                if (text.startsWith(prefix))
                    return fail(QStringLiteral("清残留条目没生效：%1").arg(text));
            }
        }
    }
    const int rowsBefore = list->count();

    // ── 1. 新建走独立对话框，确定后自动写回库文件 ───────────────────────────
    driveEditor(customName, customDefinition, true);
    newButton->click();
    waitFor(200);
    if (list->count() != rowsBefore + 1)
        return fail(QStringLiteral("新建条目没进列表：%1").arg(status->text()));
    if (int(library->entries().size()) != list->count()) {
        return fail(QStringLiteral("面板列表和库条目数不一致：面板 %1，库 %2")
                        .arg(list->count())
                        .arg(int(library->entries().size())));
    }
    {
        // 面板上已经没有「写入库文件」了：新建完必须已经落盘。
        QFile file(libraryPath);
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("新建后库文件读不回来：%1").arg(libraryPath));
        const QString json = QString::fromUtf8(file.readAll());
        file.close();
        if (!json.contains(customName) || !json.contains(QStringLiteral("+proj=tmerc")))
            return fail(QStringLiteral("新建没有自动写回库文件：%1").arg(libraryPath));
    }

    // ── 2. 认不出来的定义：对话框的「确定」必须点不了 ───────────────────────
    driveEditor(QStringLiteral("冒烟非法条目"), QStringLiteral("+proj=nonsense_zone +whatever=1"),
                false);
    newButton->click();
    waitFor(200);
    if (editorSawEnabledOk)
        return fail(QStringLiteral("非法定义时「确定」还是可点的"));
    if (list->count() != rowsBefore + 1)
        return fail(QStringLiteral("非法定义被存进了库（%1 行）").arg(list->count()));

    // ── 3. 双击 = 设为显示参照系：开关打开、投影真生效 ──────────────────────
    int row = -1;
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->text().startsWith(customName)) {
            row = index;
            break;
        }
    }
    if (row < 0)
        return fail(QStringLiteral("列表里找不到刚建的条目"));
    list->setCurrentRow(row);
    waitFor(80);
    reference->resetOrigin();
    // 面板里去掉了「设为显示参照系」按钮，双击才是启用的入口。
    QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                              Q_ARG(QListWidgetItem *, list->item(row)));
    waitFor(150);
    if (!reference->dynamicProjectionEnabled()) {
        return fail(QStringLiteral("双击启用参照系后动态投影开关没打开（状态：%1，当前显示参照系：%2）")
                        .arg(status->text(), reference->displayCrs()));
    }
    // 面板按 effectiveDefinition()（权威码优先，其次用户给的定义文本）交给后端，
    // 所以这里不比字符串相等，只确认确实换成了刚建的那条。
    const QString appliedCrs = reference->displayCrs().trimmed();
    if (appliedCrs.isEmpty() || appliedCrs == QLatin1String("EPSG:4979")) {
        return fail(QStringLiteral("显示参照系没切过来：当前 %1（状态：%2）")
                        .arg(reference->displayCrs(), status->text()));
    }
    // 在 117°E·30°N（自定义带中央经线 111°E 以东 6°）收敛角应约 3°。
    if (!reference->setOrigin(117.0, 30.0, 0.0))
        return fail(QStringLiteral("冒烟用例钉不住世界原点"));
    View3D::IGeoReference::PlaneFrame frame {};
    if (!reference->planeFrameAt(reference->displayCrs(), 117.0, 30.0, 0.0, &frame))
        return fail(QStringLiteral("自定义参照系求不出平面微分：%1").arg(reference->lastError()));
    if (frame.convergenceDeg < 2.5 || frame.convergenceDeg > 3.5)
        return fail(QStringLiteral("自定义 CM111 带在 117°E 的收敛角应约 3°，实为 %1")
                        .arg(frame.convergenceDeg, 0, 'f', 3));

    // ── 4. Ribbon 下拉联动：库条目要出现在「显示参照系」里 ─────────────────
    auto *combo = window->findChild<QComboBox *>(QStringLiteral("cmd.crs.display"));
    if (!combo)
        return fail(QStringLiteral("「显示参照系」下拉没建出来，验不了库条目联动"));
    waitFor(120);   // 库变更是通过状态刷新广播传导到下拉的
    const int comboIndex = combo->findData(appliedCrs);
    if (comboIndex < 0) {
        return fail(QStringLiteral("库里的条目没进「显示参照系」下拉：当前值 %1（状态：%2）")
                        .arg(reference->displayCrs(), status->text()));
    }

    // ── 5. 下拉的「选择参照系…」：从 EPSG 全库里挑，不再只有写死的那几条 ────
    const int chooseIndex = combo->findData(QStringLiteral("choose"));
    if (chooseIndex < 0)
        return fail(QStringLiteral("「显示参照系」下拉里没有「选择参照系…」"));
    reference->resetOrigin();
    if (!reference->setOrigin(117.0, 30.0, 0.0))
        return fail(QStringLiteral("冒烟用例钉不住世界原点"));
    driveSelector(QStringLiteral("CGCS2000 114E"), true);
    combo->setCurrentIndex(chooseIndex);
    waitFor(400);   // 选择器确定后还要等宿主回读一次命令状态
    if (!selectorSawOk)
        return fail(QStringLiteral("选择器里选中条目后「确定」还是灰的"));
    if (reference->displayCrs() != QLatin1String("EPSG:4547") ||
        !reference->dynamicProjectionEnabled()) {
        return fail(QStringLiteral("选择器确定的参照系没生效：当前 %1（开关 %2）")
                        .arg(reference->displayCrs())
                        .arg(reference->dynamicProjectionEnabled() ? 1 : 0));
    }
    View3D::IGeoReference::PlaneFrame pickerFrame {};
    if (!reference->planeFrameAt(reference->displayCrs(), 117.0, 30.0, 0.0, &pickerFrame))
        return fail(QStringLiteral("选择器选出的 EPSG:4547 求不出平面微分：%1")
                        .arg(reference->lastError()));
    if (pickerFrame.convergenceDeg < 1.2 || pickerFrame.convergenceDeg > 1.8) {
        return fail(QStringLiteral("选择器选出的 EPSG:4547 收敛角应约 1.5°，实为 %1")
                        .arg(pickerFrame.convergenceDeg, 0, 'f', 3));
    }

    // ── 6. 编辑：改名 + 改定义，标识不能变（下拉和图层都按 id 认它） ────────
    const QString editedName = customName + QStringLiteral("（改）");
    QString editedDefinition = customDefinition;
    editedDefinition.replace(QStringLiteral("lon_0=111"), QStringLiteral("lon_0=114"));
    row = -1;
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->text().startsWith(customName)) {
            row = index;
            break;
        }
    }
    if (row < 0) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("编辑前在列表里找不到刚建的条目"));
    }
    const QString entryId = rowLibraryId(row);
    list->setCurrentRow(row);
    waitFor(60);
    // 编辑也走同一个对话框：右键「编辑…」→ 改名字与定义 → 确定。
    driveEditor(editedName, editedDefinition, true);
    editAction->trigger();
    waitFor(200);
    if (list->count() != rowsBefore + 1) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("编辑条目变成了新增条目（%1 行，期望 %2 行；状态：%3）")
                        .arg(list->count())
                        .arg(rowsBefore + 1)
                        .arg(status->text()));
    }
    int editedRow = -1;
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->text().startsWith(editedName)) {
            editedRow = index;
            break;
        }
    }
    if (editedRow < 0 || rowLibraryId(editedRow) != entryId) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("编辑后名字或标识没对上（状态：%1）").arg(status->text()));
    }
    bool editedInLibrary = false;
    for (const View3D::CrsLibraryEntry &entry : library->entries()) {
        if (entry.id == entryId && entry.name == editedName &&
            entry.definition.contains(QStringLiteral("lon_0=114")))
            editedInLibrary = true;
    }
    if (!editedInLibrary) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("编辑没有写进库（状态：%1）").arg(status->text()));
    }

    // ── 7. 导入 / 导出：文件通路走一遍 ─────────────────────────────────────
    QTemporaryDir fixtures;
    if (!fixtures.isValid())
        return fail(QStringLiteral("建不出导入导出夹具目录"));
    const auto writeFixture = [](const QString &path, const char *text) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        file.write(text);
        return true;
    };
    const QString prjPath = QDir(fixtures.path()).filePath(QStringLiteral("冒烟UTM51N.prj"));
    const QString badPath = QDir(fixtures.path()).filePath(QStringLiteral("冒烟坏定义.prj"));
    if (!writeFixture(prjPath, "+proj=utm +zone=51 +datum=WGS84 +units=m +no_defs") ||
        !writeFixture(badPath, "这不是参照系定义"))
        return fail(QStringLiteral("写不出导入夹具文件"));
    QStringList importFailed;
    QString importError;
    const int imported = library->importFiles({prjPath, badPath}, &importFailed, &importError);
    if (imported != 1 || importFailed.size() != 1) {
        return fail(QStringLiteral("导入应当收 1 条、拒 1 条，实收 %1、拒 %2（%3）")
                        .arg(imported)
                        .arg(importFailed.size())
                        .arg(importError));
    }
    QStringList repeatFailed;
    QString repeatError;
    if (library->importFiles({prjPath}, &repeatFailed, &repeatError) != 0)
        return fail(QStringLiteral("同一个 .prj 导第二次应当跳过，却再加了一条"));
    QString importedId;
    for (const View3D::CrsLibraryEntry &entry : library->entries()) {
        if (entry.definition.contains(QStringLiteral("+proj=utm +zone=51")))
            importedId = entry.id;
    }
    if (importedId.isEmpty())
        return fail(QStringLiteral("导入的条目没进库（状态：%1）").arg(status->text()));
    // 单条 .prj 导出必须交出别的软件能读的定义文本：纯 proj 串那条以前会写成
    // PROJ 的 CONVERSION[…] 片段，那种文件在 GIS 里是废的。
    const QString exportPath = QDir(fixtures.path()).filePath(QStringLiteral("冒烟导出.prj"));
    QString exportError;
    if (!library->exportFile(exportPath, { importedId }, &exportError))
        return fail(QStringLiteral("导出 .prj 失败：%1").arg(exportError));
    QFile exported(exportPath);
    if (!exported.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("导出的 .prj 读不回来：%1").arg(exportPath));
    const QString exportedText = QString::fromUtf8(exported.readAll()).trimmed();
    exported.close();
    if (exportedText.isEmpty() || exportedText.startsWith(QStringLiteral("CONVERSION[")))
        return fail(QStringLiteral("导出的 .prj 不是可用的参照系文本：%1")
                        .arg(exportedText.left(80)));
    for (const View3D::CrsLibraryEntry &entry : library->entries()) {
        if (entry.id == entryId && entry.wkt.startsWith(QStringLiteral("CONVERSION[")))
            return fail(QStringLiteral("纯 proj 串的条目把 CONVERSION 片段当成了 WKT：%1")
                            .arg(entry.wkt.left(80)));
    }
    // 导入那条只是用来验文件通路，别跟着写进测试库文件。
    library->remove(importedId);

    // ── 8. 删除（确认框是无头跑替用户点的） ────────────────────────────────
    // 删除前重新定位行：中间任何一次刷新都可能改变行号。
    row = -1;
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->text().startsWith(customName)) {
            row = index;
            break;
        }
    }
    if (row < 0) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("删除前在列表里找不到刚建的条目（当前 %1 行）").arg(list->count()));
    }
    list->setCurrentRow(row);
    waitFor(60);
    acceptDeleteDialog();
    deleteAction->trigger();
    waitFor(200);
    const bool deleted = list->count() == rowsBefore;
    reference->setDynamicProjectionEnabled(false);
    reference->resetOrigin();
    if (!deleted)
        return fail(QStringLiteral("删除条目没有生效（%1 行，期望 %2 行；状态：%3）")
                        .arg(list->count())
                        .arg(rowsBefore)
                        .arg(status->text()));
    {
        // 删除要立刻落盘（面板上已经没有「写入库文件」），否则下次开机又冒出同一条；
        // 只验文件通路的导入条目也不该留下。
        QFile file(libraryPath);
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("删除后库文件读不回来：%1").arg(libraryPath));
        const QString json = QString::fromUtf8(file.readAll());
        file.close();
        if (json.contains(customName))
            return fail(QStringLiteral("删除没有写回库文件：%1").arg(libraryPath));
        if (json.contains(QStringLiteral("+proj=utm +zone=51")))
            return fail(QStringLiteral("只验文件通路的导入条目被写进了库：%1").arg(libraryPath));
    }

    // ── 9. 内置参照系：枚举、分组、搜索、详情 ──────────────────────────────
    if (!browser)
        return fail(QStringLiteral("内置参照系服务没发布，验不了 EPSG 内置列表"));
    const QList<View3D::ICrsBrowser::Group> &groups = browser->groups();
    int builtinTotal = 0;
    bool hasGaussKrugerCm = false;
    bool geographic3dHas4979 = false;
    bool geographic2dHas4326 = false;
    for (const View3D::ICrsBrowser::Group &group : groups) {
        builtinTotal += group.items.size();
        if (group.id == QLatin1String("builtin.tm.gk_cm"))
            hasGaussKrugerCm = true;
        for (const View3D::ICrsBrowser::Item &item : group.items) {
            // EPSG:4979 的官方名也叫 "WGS 84"，只能靠 PROJ 的类型分 2D/3D。
            if (item.key == QLatin1String("EPSG:4979") &&
                group.id == QLatin1String("builtin.geographic3d"))
                geographic3dHas4979 = true;
            if (item.key == QLatin1String("EPSG:4326") &&
                group.id == QLatin1String("builtin.geographic2d"))
                geographic2dHas4326 = true;
        }
    }
    if (groups.size() < 3 || builtinTotal < 5000) {
        return fail(QStringLiteral("内置参照系只有 %1 条（分 %2 组）：%3")
                        .arg(builtinTotal)
                        .arg(groups.size())
                        .arg(browser->statusText()));
    }
    if (!hasGaussKrugerCm)
        return fail(QStringLiteral("内置分组里没有「高斯-克吕格 · 中央经线」这一组"));
    if (!geographic3dHas4979 || !geographic2dHas4326)
        return fail(QStringLiteral("地理系 2D/3D 分组放错了（4979 在 3D=%1，4326 在 2D=%2）")
                        .arg(geographic3dHas4979 ? 1 : 0)
                        .arg(geographic2dHas4326 ? 1 : 0));
    if (!selectTreeNode(QStringLiteral("EPSG 内置")))
        return fail(QStringLiteral("左树里没有「EPSG 内置」分支"));
    search->setText(QStringLiteral("CGCS2000 114E"));
    waitFor(250);
    const QString builtinKey = QStringLiteral("EPSG:4547");
    int builtinRow = findRow(builtinKey);
    if (builtinRow < 0) {
        return fail(QStringLiteral("搜「CGCS2000 114E」没命中 EPSG:4547（列表 %1 行，状态：%2）")
                        .arg(list->count())
                        .arg(status->text()));
    }
    const QString detailText = details->toPlainText();
    if (!detailText.contains(QStringLiteral("CGCS2000")) ||
        !detailText.contains(QStringLiteral("投影方法")) ||
        !detailText.contains(QStringLiteral("使用范围"))) {
        return fail(QStringLiteral("内置条目的详情不全：%1").arg(detailText.left(200)));
    }

    // ── 10. 内置只读：右键菜单里的「编辑 / 删除」必须是灰的 ─────────────────
    const int entriesBeforePromote = int(library->entries().size());
    list->setCurrentRow(builtinRow);
    waitFor(60);
    if (deleteAction->isEnabled())
        return fail(QStringLiteral("内置参照系的「删除」没有被禁掉"));
    if (editAction->isEnabled())
        return fail(QStringLiteral("内置参照系的「编辑」没有被禁掉"));
    deleteAction->trigger();   // 灰着的动作触发也不该有副作用
    waitFor(80);
    if (int(library->entries().size()) != entriesBeforePromote)
        return fail(QStringLiteral("删内置参照系改动了自定义库（%1 条）")
                        .arg(int(library->entries().size())));

    // ── 11. 一键启用内置：EPSG:4547 的 CM 是 114°E，117°E·30°N 收敛角约 1.5° ──
    reference->resetOrigin();
    if (!reference->setOrigin(117.0, 30.0, 0.0))
        return fail(QStringLiteral("冒烟用例钉不住世界原点"));
    QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                              Q_ARG(QListWidgetItem *, list->item(builtinRow)));
    waitFor(150);
    if (!reference->dynamicProjectionEnabled())
        return fail(QStringLiteral("启用内置参照系后动态投影开关没打开（状态：%1）")
                        .arg(status->text()));
    View3D::IGeoReference::PlaneFrame builtinFrame {};
    if (!reference->planeFrameAt(builtinKey, 117.0, 30.0, 0.0, &builtinFrame))
        return fail(QStringLiteral("内置 EPSG:4547 求不出平面微分：%1").arg(reference->lastError()));
    if (builtinFrame.convergenceDeg < 1.2 || builtinFrame.convergenceDeg > 1.8)
        return fail(QStringLiteral("EPSG:4547（CM 114E）在 117°E·30°N 收敛角应约 1.5°，实为 %1")
                        .arg(builtinFrame.convergenceDeg, 0, 'f', 3));

    // ── 12. 收藏与下拉联动：撤掉一条常用，下拉里也该少一条 ─────────────────
    const QString favouriteKey = QStringLiteral("EPSG:4490");   // 默认常用里有，XML 下拉里没有
    search->setText(favouriteKey);
    waitFor(250);
    int favouriteRow = findRow(favouriteKey);
    if (favouriteRow < 0)
        return fail(QStringLiteral("搜 EPSG:4490 没命中内置条目（列表 %1 行）").arg(list->count()));
    list->setCurrentRow(favouriteRow);
    waitFor(60);
    const bool wasFavorite = library->isFavorite(favouriteKey);
    favoriteAction->trigger();
    waitFor(200);
    if (library->isFavorite(favouriteKey) != !wasFavorite)
        return fail(QStringLiteral("点「收藏」没改变收藏状态（状态：%1）").arg(status->text()));
    const bool stillInCombo = combo->findData(favouriteKey) >= 0;
    if (stillInCombo == wasFavorite) {
        return fail(QStringLiteral("收藏状态变了但下拉没跟着变（原来收藏=%1，现在下拉命中=%2）")
                        .arg(wasFavorite ? 1 : 0)
                        .arg(stillInCombo ? 1 : 0));
    }
    favoriteAction->trigger();   // 复原，别把默认常用集改坏了
    waitFor(200);
    if (library->isFavorite(favouriteKey) != wasFavorite ||
        combo->findData(favouriteKey) < 0)
        return fail(QStringLiteral("收藏复原没生效（状态下拉命中 %1）")
                        .arg(combo->findData(favouriteKey)));

    // ── 13. 存进自定义：内置那条固化成可编辑记录，再撤回 ───────────────────
    search->setText(builtinKey);
    waitFor(250);
    builtinRow = findRow(builtinKey);
    if (builtinRow < 0)
        return fail(QStringLiteral("第二次搜 EPSG:4547 没命中（列表 %1 行）").arg(list->count()));
    list->setCurrentRow(builtinRow);
    waitFor(60);
    // 「存进自定义…」会弹同一个对话框，带着内置的官方名与 EPSG 码：直接确定。
    driveEditor(QString(), QString(), true);
    promoteAction->trigger();
    waitFor(200);
    if (int(library->entries().size()) != entriesBeforePromote + 1) {
        return fail(QStringLiteral("「存进自定义」没新增记录（库 %1 条，状态：%2）")
                        .arg(int(library->entries().size()))
                        .arg(status->text()));
    }
    bool promotedNamed = false;
    for (const View3D::CrsLibraryEntry &entry : library->entries()) {
        if (entry.definition == builtinKey && entry.name.contains(QStringLiteral("CGCS2000")))
            promotedNamed = true;
    }
    if (!promotedNamed)
        return fail(QStringLiteral("固化出来的库记录没带官方名（状态：%1）").arg(status->text()));
    if (!selectTreeNode(QStringLiteral("自定义")))
        return fail(QStringLiteral("存进自定义后回不到「自定义」分支"));
    const int promotedRow = findRow(builtinKey);
    if (promotedRow < 0) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("自定义列表里找不到刚固化的那条"));
    }
    list->setCurrentRow(promotedRow);
    waitFor(60);
    acceptDeleteDialog();
    deleteAction->trigger();
    waitFor(200);
    if (int(library->entries().size()) != entriesBeforePromote) {
        reference->setDynamicProjectionEnabled(false);
        reference->resetOrigin();
        return fail(QStringLiteral("撤回固化记录没生效（库 %1 条，期望 %2 条）")
                        .arg(int(library->entries().size()))
                        .arg(entriesBeforePromote));
    }
    reference->setDynamicProjectionEnabled(false);
    reference->resetOrigin();

    // ── 14. 排版体检：把面板真停靠显示出来，量三栏是否都还活着 ─────────────
    // 宿主把 CDockWidget 用同一个名字注册进 UI 对象表（面板内容widget也叫这名，
    // findChild 只能拿到先遇到的那个），所以要按注册表取，别靠 parent 链找。
    auto *registry = application.getUiObjectRegistry();
    auto *panel = registry
        ? qobject_cast<ads::CDockWidget *>(registry->widget(QStringLiteral("dw.crs_manager")))
        : nullptr;
    if (!panel)
        return fail(QStringLiteral("参照系面板的宿主 CDockWidget 取不到，量不了排版"));
    panel->toggleView(true);
    QApplication::processEvents();
    waitFor(400);
    const struct {
        QWidget *widget;
        const char *name;
    } panes[] = { { tree, "tree" }, { list, "list" }, { details, "details" } };
    // 面板内容 widget 与 CDockWidget 同名，所以从 CDockWidget 往下找（它不会找到自已）。
    QWidget *content = panel->findChild<QWidget *>(QStringLiteral("dw.crs_manager"));
    if (!content) {
        panel->toggleView(false);
        return fail(QStringLiteral("参照系面板的内容 widget 找不到，量不了排版"));
    }
    // 结构断言：三栏必须都在面板内容里，任何一栏没挂上都是「面板不好用」的根。
    for (const auto &pane : panes) {
        if (!content->isAncestorOf(pane.widget)) {
            panel->toggleView(false);
            return fail(QStringLiteral("%1 不在面板内容里").arg(QString::fromLatin1(pane.name)));
        }
    }
    content->resize(980, 520);
    if (auto *layout = content->layout()) {
        layout->invalidate();
        layout->activate();
    }
    // 列表与详情各套了一层容器，容器自己的布局也要激活一次才算完。
    const auto activateLayouts = [](QWidget *root) {
        for (QWidget *widget : root->findChildren<QWidget *>()) {
            if (auto *layout = widget->layout()) {
                layout->invalidate();
                layout->activate();
            }
        }
    };
    activateLayouts(content);
    activateLayouts(content);   // 第二遍让内层容器按新尺寸再排一次
    QApplication::processEvents();
    waitFor(150);
    QString paneSizes;
    bool laidOut = true;
    for (const auto &pane : panes) {
        paneSizes += QStringLiteral(" %1=%2x%3")
                         .arg(QString::fromLatin1(pane.name))
                         .arg(pane.widget->width())
                         .arg(pane.widget->height());
        laidOut = laidOut && pane.widget->width() >= 80 && pane.widget->height() >= 60;
    }
    if (!laidOut) {
        std::cerr << "VIEW3D_CRS_LAYOUT_SKIPPED reason=window_hidden panes="
                  << paneSizes.toStdString() << std::endl;
    }
    // 详情区得有内容：文本版渲染在隐藏控件上量不出像素（QTextEdit 的 viewport
    // 尺寸要等 resizeEvent），所以这里查它拿到的文本，字段完整性在上面第 9 步查。
    if (details->toPlainText().trimmed().isEmpty()) {
        panel->toggleView(false);
        return fail(QStringLiteral("详情区是空的，选中项没喂给详情面板"));
    }
    const QSize panelSize = content->size();
    panel->toggleView(false);
    QApplication::processEvents();

    // ── 15. 详情区的「复制 proj4」要真的把定义送进剪贴板 ───────────────────
    if (!selectTreeNode(QStringLiteral("EPSG 内置")))
        return fail(QStringLiteral("排版体检后回不到「EPSG 内置」分支"));
    search->setText(builtinKey);
    waitFor(250);
    const int copyRow = findRow(builtinKey);
    if (copyRow < 0)
        return fail(QStringLiteral("找不到 EPSG:4547，验不了复制按钮"));
    list->setCurrentRow(copyRow);
    waitFor(80);
    if (!copyProj->isEnabled())
        return fail(QStringLiteral("详情里有 proj4 时「复制 proj4」却还是灰的"));
    QApplication::clipboard()->clear();
    copyProj->click();
    const QString copied = QApplication::clipboard()->text();
    if (!copied.startsWith(QStringLiteral("+proj=")))
        return fail(QStringLiteral("「复制 proj4」没把定义放进剪贴板：%1").arg(copied.left(60)));

    std::cerr << "VIEW3D_CRS_LIB_OK rows=" << list->count() << " conv="
              << frame.convergenceDeg << " dropdown=" << comboIndex
              << " builtin=" << builtinTotal << " groups=" << groups.size()
              << " epsgConv=" << builtinFrame.convergenceDeg
              << " panel=" << panelSize.width() << "x" << panelSize.height()
              << " panes=" << paneSizes.toStdString()
              << " library=" << libraryPath.toStdString() << std::endl;
    return true;
}

// 「打开模型」冒烟：运行时生成 OBJ/GLTF/LAS/KML 四个小夹具，只通过数据插件登记到
// 通用「打开数据」注册表的 IDataOpener 打开，验证格式探测、异步导入、图层落位与
// 上屏。夹具都用裸字节/文本写出来，不依赖任何导入库，因此宿主侧不需要链接它们。
bool verifyModelImportSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *openRegistry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!scene || !layers || !openRegistry)
        return fail(QStringLiteral("模型导入冒烟缺少场景或打开数据注册表"));

    View3D::IDataOpener *modelOpener = nullptr;
    View3D::IDataOpener *cloudOpener = nullptr;
    View3D::IDataOpener *vectorOpener = nullptr;
    View3D::IDataOpener *terrainOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (!opener)
            continue;
        const QString name = opener->displayName();
        if (!modelOpener && name.contains(QStringLiteral("模型文件")))
            modelOpener = opener;
        if (!cloudOpener && name.contains(QStringLiteral("点云")))
            cloudOpener = opener;
        if (!vectorOpener && name.contains(QStringLiteral("矢量")))
            vectorOpener = opener;
        // 「栅格 / 地形」这一行就是 DEM/GeoTIFF 的入口（地形不再走在线地图页按钮）。
        if (!terrainOpener && name.contains(QStringLiteral("地形")))
            terrainOpener = opener;
    }
    if (!modelOpener) {
        // 没有任何模型读取器被编进插件（缺 assimp/OSG/Cesium）时不算失败。
        std::cerr << "VIEW3D_MODEL_IMPORT_SKIPPED reason=no_model_opener" << std::endl;
        return true;
    }

    QTemporaryDir directory;
    if (!directory.isValid())
        return fail(QStringLiteral("无法创建模型导入夹具目录"));
    const QString root = directory.path();

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    // 导入选项对话框是模态的：无头跑必须在下一轮事件循环里替用户点确定。
    const auto acceptImportDialog = [] {
        QTimer::singleShot(0, [] {
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (dialog && dialog->isModal() && dialog->isVisible() &&
                    dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                    dialog->accept();
                    return;
                }
            }
        });
    };
    const auto modelKeys = [scene] {
        QSet<QString> keys;
        for (const QString &key : scene->modelLayerKeys())
            keys.insert(key);
        return keys;
    };
    // 进度条是非模态的，失败提示是模态的：按标题找顶层对话框，避免依赖插件私有类型。
    const auto findModelDialog = [](const QString &title, bool modalOnly) -> QDialog * {
        const auto widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || !dialog->isVisible() || !dialog->windowTitle().contains(title))
                continue;
            if (modalOnly && !dialog->isModal())
                continue;
            return dialog;
        }
        return nullptr;
    };
    const QString progressTitle = QStringLiteral("正在打开数据");
    bool progressSeen = false;
    bool progressGone = false;
    // 打开一个文件并等它真正变成图层；返回新图层的渲染键（空串表示没出来）。
    // 图层必须落在传入的组里，收尾时删组才会连带释放顶点。
    const auto openAndWait = [&](View3D::IDataOpener *opener, const QString &path,
                                 const View3D::LayerId &group) -> QString {
        const QSet<QString> before = modelKeys();
        acceptImportDialog();
        QString openError;
        if (!opener->openData(path, group, &openError)) {
            qWarning().noquote() << "openData refused" << path << openError;
            return {};
        }
        // 用户选完文件就该立刻看到进度，否则大文件读几十秒像卡死。
        progressSeen = progressSeen || findModelDialog(progressTitle, false) != nullptr;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 8000) {
            waitFor(50);
            for (const QString &key : scene->modelLayerKeys()) {
                if (!before.contains(key)) {
                    progressGone = findModelDialog(progressTitle, false) == nullptr;
                    return key;
                }
            }
        }
        return {};
    };
    const auto meshVertices = [scene](const QString &key) -> std::size_t {
        const auto mesh = scene->modelLayerMesh(key);
        return mesh ? mesh->vertices.size() : 0;
    };

    const View3D::LayerId importGroup = layers->addGroup(QStringLiteral("模型导入测试组"));
    if (importGroup.isNull())
        return fail(QStringLiteral("无法创建模型导入测试组"));

    const std::uint64_t baseVertices = scene->registeredVertexCount();
    const QSize shotSize(320, 200);
    QImage background;
    View3D::IRenderSurface *view = layout ? layout->activeView() : nullptr;
    if (view)
        background = view->captureImageOffscreen(shotSize, View3D::CaptureOptions());

    // ── OBJ：两个三角形拼一个方块，带法线与 uv，走 assimp 通路 ──────────────
    const QString objPath = QDir(root).filePath(QStringLiteral("fixture.obj"));
    {
        QFile file(objPath);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 OBJ 夹具"));
        file.write(
            "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
            "vn 0 0 1\n"
            "s off\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n");
    }
    const QString objKey = openAndWait(modelOpener, objPath, importGroup);
    if (objKey.isEmpty())
        return fail(QStringLiteral("OBJ 未通过通用打开数据生成图层"));
    if (meshVertices(objKey) < 4)
        return fail(QStringLiteral("OBJ 图层顶点数不足：%1").arg(meshVertices(objKey)));
    {
        const auto mesh = scene->modelLayerMesh(objKey);
        bool hasNormal = false;
        for (const View3D::MeshVertexDto &vertex : mesh->vertices)
            hasNormal = hasNormal || vertex.hasNormal;
        if (!hasNormal)
            return fail(QStringLiteral("OBJ 图层丢失法线（PBS 会输出纯白）"));
    }

    // ── OSGT 文本场景：一个三角形，走本插件自己的 osgDB 通路（assimp 不认 .osgt）──
    // 语法照本机倾斜摄影成果里的 .osgt 样例（#Ascii Scene / osg::X { UniqueID n … }），
    // 自己臆造的 osgGroup{Object…} 旧式写法 osgdb_osg 并不接受。
    QString osgKey;
    {
        QFile file(QDir(root).filePath(QStringLiteral("fixture.osgt")));
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 OSG 夹具"));
        file.write(
            // 头部与属性顺序都照本机倾斜摄影成果里的 .osgt 逐行同形（含行尾空格与
            // CRLF）：osgdb_osg 的 ASCII 解析器按写出顺序读属性，把 PrimitiveSetList
            // 放到数组之后就读不到画元组，顶点却读得到，表现为"没有可直接显示的几何"。
            "#Ascii Scene \r\n"
            "#Version 161 \r\n"
            "#Generator View3D 1.0 \r\n"
            "\r\n"
            "osg::Geode {\r\n"
            "  UniqueID 1 \r\n"
            "  Drawables 1 {\r\n"
            "    osg::Geometry {\r\n"
            "      UniqueID 2 \r\n"
            "      DataVariance STATIC \r\n"
            "      PrimitiveSetList 1 {\r\n"
            "        osg::DrawElementsUInt {\r\n"
            "          UniqueID 3 \r\n"
            "          Mode TRIANGLES \r\n"
            "          vector 3 {\r\n"
            "            0 1 2 \r\n"
            "          }\r\n"
            "        }\r\n"
            "      }\r\n"
            "      VertexArray TRUE {\r\n"
            "        osg::Vec3Array {\r\n"
            "          UniqueID 4 \r\n"
            "          Binding BIND_PER_VERTEX \r\n"
            "          vector 3 {\r\n"
            "            -1 -1 0 \r\n"
            "            1 -1 0 \r\n"
            "            0 1 0 \r\n"
            "          }\r\n"
            "        }\r\n"
            "      }\r\n"
            "    }\r\n"
            "  }\r\n"
            "}\r\n");
        file.close();
        osgKey = openAndWait(modelOpener, QDir(root).filePath(QStringLiteral("fixture.osgt")),
                             importGroup);
        if (osgKey.isEmpty())
            return fail(QStringLiteral(".osgt 未通过本插件的 osgDB 通路生成图层"));
        if (meshVertices(osgKey) < 3)
            return fail(QStringLiteral("OSG 图层顶点数不足：%1").arg(meshVertices(osgKey)));
    }

    // ── GLTF + 外部 buffer：专门覆盖 .gltf 的外部资源解析（不落网络）────────
    QString gltfKey;
    {
        const QString binPath = QDir(root).filePath(QStringLiteral("buffer.bin"));
        QByteArray payload;
        const float positions[9] = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const quint16 indices[3] = {0, 1, 2};
        payload.append(reinterpret_cast<const char *>(positions),
                       static_cast<qsizetype>(sizeof(positions)));
        payload.append(reinterpret_cast<const char *>(indices),
                       static_cast<qsizetype>(sizeof(indices)));
        QFile binFile(binPath);
        if (!binFile.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 glTF 外部 buffer"));
        binFile.write(payload);
        binFile.close();

        QFile file(QDir(root).filePath(QStringLiteral("fixture.gltf")));
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 glTF 夹具"));
        // buffer.bin 的前 36 字节是 POSITION，紧接着 6 字节是索引；byteLength 必须
        // 与实际写入一致，否则 Cesium 的读取器会把整个文件判为损坏。
        file.write(
            "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
            "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
            "{\"POSITION\":0},\"indices\":1}]}],\"buffers\":[{\"uri\":\"buffer.bin\","
            "\"byteLength\":42}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
            "\"byteLength\":36,\"target\":34962},{\"buffer\":0,\"byteOffset\":36,"
            "\"byteLength\":6,\"target\":34963}],\"accessors\":[{\"bufferView\":0,"
            "\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1.0,-1.0,0.0],"
            "\"max\":[1.0,1.0,0.0]},{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
            "\"type\":\"SCALAR\"}]}");
        file.close();
        gltfKey = openAndWait(modelOpener, QDir(root).filePath(QStringLiteral("fixture.gltf")), importGroup);
        if (gltfKey.isEmpty())
            return fail(QStringLiteral("外部 buffer 的 .gltf 未生成图层"));
        if (meshVertices(gltfKey) < 3)
            return fail(QStringLiteral("glTF 图层顶点数不足：%1").arg(meshVertices(gltfKey)));
    }

    // ── LAS：裸写 LAS 1.2 格式 2（带 RGB）三个点，走 LASzip 点云通路 ─────────
    QString lasKey;
    if (cloudOpener) {
        const QByteArray las = buildLasPointCloudFixture();
        QFile file(QDir(root).filePath(QStringLiteral("fixture.las")));
        if (!file.open(QIODevice::WriteOnly) || file.write(las) != las.size())
            return fail(QStringLiteral("无法写 LAS 夹具"));
        file.close();
        lasKey = openAndWait(cloudOpener, QDir(root).filePath(QStringLiteral("fixture.las")), importGroup);
        if (lasKey.isEmpty())
            return fail(QStringLiteral("LAS 点云未生成图层"));
        const auto mesh = scene->modelLayerMesh(lasKey);
        if (mesh->vertices.size() != 3)
            return fail(QStringLiteral("LAS 图层点数应为 3，实际 %1").arg(mesh->vertices.size()));
        if (mesh->primitives.empty() ||
            mesh->primitives.front().topology != View3D::ModelPrimitiveTopology::Points)
            return fail(QStringLiteral("LAS 图层不是点拓扑"));
    }

    // ── KML：一条折线，走 GDAL/OGR 矢量通路 ─────────────────────────────────
    QString kmlKey;
    if (vectorOpener) {
        QFile file(QDir(root).filePath(QStringLiteral("fixture.kml")));
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 KML 夹具"));
        file.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark>"
                   "<LineString><coordinates>0,0,0 1,1,0 2,0,0</coordinates></LineString>"
                   "</Placemark></Document></kml>\n");
        file.close();
        kmlKey = openAndWait(vectorOpener, QDir(root).filePath(QStringLiteral("fixture.kml")), importGroup);
        if (kmlKey.isEmpty())
            return fail(QStringLiteral("KML 矢量未生成图层"));
        if (meshVertices(kmlKey) == 0)
            return fail(QStringLiteral("KML 图层没有几何"));
    }

    // ── GeoTIFF 地形：单波段栅格要生成随高程起伏的地形网格 ──────────────────
    QString demKey;
    if (terrainOpener) {
        const QString demPath = QDir(root).filePath(QStringLiteral("fixture_dem.tif"));
        QFile demFile(demPath);
        if (!demFile.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写 GeoTIFF 夹具"));
        const QByteArray tiff = buildSmokeGeoTiff();
        if (demFile.write(tiff) != tiff.size()) {
            demFile.close();
            return fail(QStringLiteral("GeoTIFF 夹具写入不完整"));
        }
        demFile.close();
        demKey = openAndWait(terrainOpener, demPath, importGroup);
        if (demKey.isEmpty())
            return fail(QStringLiteral("GeoTIFF 地形未生成图层"));
        const auto demMesh = scene->modelLayerMesh(demKey);
        if (!demMesh || demMesh->vertices.size() < 16)
            return fail(QStringLiteral("地形网格顶点过少：%1")
                            .arg(demMesh ? demMesh->vertices.size() : 0));
        double minHeight = std::numeric_limits<double>::max();
        double maxHeight = std::numeric_limits<double>::lowest();
        for (const View3D::MeshVertexDto &vertex : demMesh->vertices) {
            minHeight = std::min(minHeight, vertex.y);
            maxHeight = std::max(maxHeight, vertex.y);
        }
        // 夹具高程 0..70：显示层的 Y 就是高程，起伏没出来就说明没按 DEM 走。
        if (maxHeight - minHeight < 50.0)
            return fail(QStringLiteral("地形网格没有按高程起伏：Y 范围 %1~%2")
                            .arg(minHeight).arg(maxHeight));
        if (demMesh->textures.empty())
            return fail(QStringLiteral("地形网格缺少山体阴影贴图"));
    } else {
        std::cerr << "VIEW3D_MODEL_DEM_SKIPPED reason=no_raster_opener" << std::endl;
    }

    const int importedLayers = 1 + (osgKey.isEmpty() ? 0 : 1) + (gltfKey.isEmpty() ? 0 : 1) +
                               (lasKey.isEmpty() ? 0 : 1) + (kmlKey.isEmpty() ? 0 : 1) +
                               (demKey.isEmpty() ? 0 : 1);
    if (scene->registeredVertexCount() <= baseVertices)
        return fail(QStringLiteral("导入后场景顶点数没有增加"));

    int changedPixels = 0;
    if (view && !background.isNull()) {
        waitFor(200);
        const QImage shot = view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        const QImage left = shot.convertToFormat(QImage::Format_ARGB32);
        const QImage base = background.convertToFormat(QImage::Format_ARGB32);
        if (left.size() == base.size()) {
            for (int y = 0; y < left.height(); ++y) {
                const auto *line = reinterpret_cast<const QRgb *>(left.constScanLine(y));
                const auto *baseLine = reinterpret_cast<const QRgb *>(base.constScanLine(y));
                for (int x = 0; x < left.width(); ++x)
                    changedPixels += line[x] != baseLine[x] ? 1 : 0;
            }
        }
    }
    if (changedPixels <= 0)
        return fail(QStringLiteral("导入的模型没有上屏（changedPixels=%1）").arg(changedPixels));
    if (!progressSeen)
        return fail(QStringLiteral("选完文件后没有出现导入进度对话框"));
    if (!progressGone)
        return fail(QStringLiteral("导入结束后进度对话框没有收起"));

    // ── 坏文件：按扩展名能被认领，内容读不出来；异步失败必须弹窗告诉用户 ──────
    bool failureDialogShown = false;
    {
        const QString brokenPath = QDir(root).filePath(QStringLiteral("broken.obj"));
        QFile brokenFile(brokenPath);
        if (!brokenFile.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("无法写坏 OBJ 夹具"));
        brokenFile.write("这不是 wavefront 对象文件，只是用来触发失败的文本\n");
        brokenFile.close();
        acceptImportDialog();
        QString brokenError;
        if (!modelOpener->openData(brokenPath, importGroup, &brokenError))
            return fail(QStringLiteral("坏文件应当先被受理（失败发生在异步读取阶段）：%1")
                            .arg(brokenError));
        QMessageBox *box = nullptr;
        QElapsedTimer failureTimer;
        failureTimer.start();
        while (failureTimer.elapsed() < 8000) {
            waitFor(50);
            box = qobject_cast<QMessageBox *>(findModelDialog(QStringLiteral("打开数据失败"), false));
            if (box)
                break;
        }
        if (!box) {
            // 把当时可见的顶层窗口列出来：找不到对话框最常见的原因是标题没设上
            // 或者它不是模态窗口，只看"没弹出"这句结论没法定位。
            QString seen;
            const auto visible = QApplication::topLevelWidgets();
            for (QWidget *widget : visible) {
                if (!widget->isVisible())
                    continue;
                seen += QStringLiteral(" [%1|modal=%2|class=%3]")
                            .arg(widget->windowTitle())
                            .arg(widget->isModal() ? 1 : 0)
                            .arg(QString::fromUtf8(widget->metaObject()->className()));
            }
            return fail(QStringLiteral("打开失败没有弹出对话框，当前可见窗口:%1").arg(seen));
        }
        failureDialogShown = true;
        const QString detail = box->text() + QLatin1Char('\n') + box->informativeText();
        if (!detail.contains(QStringLiteral("broken.obj")))
            return fail(QStringLiteral("失败对话框没写是哪个文件：%1").arg(detail));
        if (box->informativeText().trimmed().isEmpty())
            return fail(QStringLiteral("失败对话框没有给出原因"));
        box->done(QMessageBox::Ok);
        waitFor(50);
        if (findModelDialog(QStringLiteral("打开数据失败"), false))
            return fail(QStringLiteral("失败对话框点确定后没有关闭"));
    }

    const std::uint64_t importedVertices =
        scene->registeredVertexCount() - baseVertices;
    layers->removeLayer(importGroup);
    QElapsedTimer releaseTimer;
    releaseTimer.start();
    while (releaseTimer.elapsed() < 20000 && scene->registeredVertexCount() > baseVertices)
        waitFor(50);
    if (scene->registeredVertexCount() != baseVertices)
        return fail(QStringLiteral("删除图层后仍占用顶点资源"));

    std::cerr << "VIEW3D_MODEL_IMPORT_OK obj=1 osg=" << (osgKey.isEmpty() ? 0 : 1)
              << " gltf=" << (gltfKey.isEmpty() ? 0 : 1)
              << " las=" << (lasKey.isEmpty() ? 0 : 1) << " kml=" << (kmlKey.isEmpty() ? 0 : 1)
              << " layers=" << importedLayers << " vertices=" << importedVertices
              << " progress=" << (progressSeen && progressGone ? 1 : 0)
              << " failure_dialog=" << (failureDialogShown ? 1 : 0)
              << " pixels=" << changedPixels << std::endl;
    return true;
}


// 属性面板与日志面板：走真实控件路径验证「树选中 → 面板回显 → 改值写进场景 →
// 外部改动回流面板」，以及 AppLog 与 qWarning 是否落到同一个可过滤的缓冲里。
bool verifyInspectorAndLogSmoke(RF::PluginEngine::IApplication &application,
                                View3DApplication::PluginUiHost &uiHost, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *service = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *treeDock = container
        ? container->objectAs<View3D::ISceneTreeDock>(View3D::ServiceIds::SceneTreeDock)
        : nullptr;
    auto *properties = container
        ? container->objectAs<RF::PluginEngine::IDockWindow>(QStringLiteral("ui.dw.properties"))
        : nullptr;
    auto *inspector = container
        ? container->objectAs<View3D::ILayerPropertiesDock>(
              View3D::ServiceIds::LayerPropertiesDock)
        : nullptr;
    auto *logWindow = container
        ? container->objectAs<RF::PluginEngine::IDockWindow>(QStringLiteral("ui.dw.log"))
        : nullptr;
    if (!service || !scene || !treeDock || !properties || !inspector || !logWindow) {
        // 面板由 ui_config 声明；换配置就不存在，这里不算失败。
        std::cerr << "VIEW3D_INSPECTOR_LOG_SKIPPED reason=panels_not_configured" << std::endl;
        return true;
    }
    auto *logDock = dynamic_cast<View3DApplication::LogDock *>(logWindow);
    if (!logDock)
        return fail(QStringLiteral("日志面板类型与配置不符"));
    QWidget *panel = properties->getWidget();
    QWidget *logWidget = logDock->getWidget();
    if (!panel || !logWidget)
        return fail(QStringLiteral("面板没有控件"));
    auto *nameEdit = panel->findChild<QLineEdit *>(QStringLiteral("layerName"));
    auto *visibleCheck = panel->findChild<QCheckBox *>(QStringLiteral("layerVisible"));
    auto *opacity = panel->findChild<QSlider *>(QStringLiteral("layerOpacity"));
    auto *edgeLines = panel->findChild<QCheckBox *>(QStringLiteral("layerEdgeLines"));
    auto *offsetX = panel->findChild<QDoubleSpinBox *>(QStringLiteral("layerOffsetX"));
    auto *minDistance = panel->findChild<QDoubleSpinBox *>(QStringLiteral("layerMinDistance"));
    auto *logFilter = logWidget->findChild<QLineEdit *>(QStringLiteral("logFilter"));
    if (!nameEdit || !visibleCheck || !opacity || !edgeLines || !offsetX || !minDistance
        || !logFilter)
        return fail(QStringLiteral("面板控件缺少对象名，无法驱动"));

    const QString renderKey = QStringLiteral("inspector:triangle");
    const View3D::LayerId layerId =
        service->addModelLayer(QStringLiteral("属性面板测试图层"), renderKey,
                               makeSmokeTriangleMesh());
    if (layerId.isNull())
        return fail(QStringLiteral("无法登记属性面板测试图层"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };

    // 选中必须经场景树走真实通路，不能直接调面板的接口。
    QTreeWidget *tree = treeDock->sceneTree();
    QTreeWidgetItem *item = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole + 1).toUuid() == layerId)
            item = *it;
    }
    if (!item)
    {
        service->removeLayer(layerId);
        return fail(QStringLiteral("测试图层没有出现在场景树"));
    }
    tree->setCurrentItem(item);
    waitFor(80);
    if (inspector->currentLayer() != layerId)
    {
        const QString treeCurrent = tree->currentItem()
            ? tree->currentItem()->data(0, Qt::UserRole + 1).toUuid().toString()
            : QStringLiteral("<no-current-item>");
        const QString treeSelected = tree->selectedItems().isEmpty()
            ? QStringLiteral("<none>")
            : tree->selectedItems().first()->data(0, Qt::UserRole + 1)
                  .toUuid()
                  .toString();
        service->removeLayer(layerId);
        return fail(QStringLiteral("场景树选中没有传到属性面板（面板=%1 树当前=%2 树选中=%3 期望=%4）")
                        .arg(inspector->currentLayer().toString(), treeCurrent, treeSelected,
                             layerId.toString()));
    }
    if (nameEdit->text() != QStringLiteral("属性面板测试图层"))
    {
        service->removeLayer(layerId);
        return fail(QStringLiteral("属性面板没有回显图层名称：%1").arg(nameEdit->text()));
    }

    // 参照系：没有参照系的模型必须留空（显示 "-"），不能编造；有矩阵时显示 4×4 与原点。
    auto *crsLabel = panel->findChild<QLabel *>(QStringLiteral("layerSourceCrs"));
    auto *frameLabel = panel->findChild<QLabel *>(QStringLiteral("layerReferenceFrame"));
    if (!crsLabel || !frameLabel) {
        service->removeLayer(layerId);
        return fail(QStringLiteral("属性面板缺少参照系/矩阵标签"));
    }
    if (crsLabel->text() != QStringLiteral("-") || frameLabel->text() != QStringLiteral("-")) {
        service->removeLayer(layerId);
        return fail(QStringLiteral("无参照系模型的参照系应为空：%1 / %2")
                        .arg(crsLabel->text(), frameLabel->text()));
    }
    {
        View3D::ILayer::ReferenceFrame sample;
        sample.hasMatrix = true;
        // Rx(-90)：源 (x,y,z) → 场景 (x+10, z+20, -y+30)。
        sample.matrix = {1.0, 0.0, 0.0, 10.0, 0.0, 0.0, 1.0, 20.0,
                         0.0, -1.0, 0.0, 30.0, 0.0, 0.0, 0.0, 1.0};
        sample.position = QStringLiteral("10.00000000°, 20.00000000°, 椭球高 30 m");
        sample.origin = QStringLiteral("SRSOrigin 1, 2, 3 m");
        service->setLayerReferenceFrame(layerId, sample);
        waitFor(80);
        const QString text = frameLabel->text();
        if (!text.contains(QStringLiteral("矩阵")) ||
            !text.contains(QStringLiteral("10.00000000°")) ||
            !text.contains(QStringLiteral("SRSOrigin 1, 2, 3 m")) ||
            text.contains(QStringLiteral("说明")) ||
            text.count(QLatin1Char('\n')) < 5) {
            service->removeLayer(layerId);
            return fail(QStringLiteral("参照系没有按 4×4 + 经纬度 + 原点显示：%1").arg(text));
        }
        View3D::ILayer::ReferenceFrame cleared;
        service->setLayerReferenceFrame(layerId, cleared);
        waitFor(50);
        if (frameLabel->text() != QStringLiteral("-")) {
            service->removeLayer(layerId);
            return fail(QStringLiteral("清空参照系后面板没有回到空显示"));
        }
    }

    // 不透明度：写进渲染场景才算通（渲染端有独立取值接口）。
    opacity->setValue(50);
    waitFor(80);
    if (qAbs(static_cast<double>(scene->modelLayerOpacity(renderKey)) - 0.50) > 0.02)
    {
        service->removeLayer(layerId);
        return fail(QStringLiteral("不透明度没有写进场景：%1")
                        .arg(scene->modelLayerOpacity(renderKey)));
    }

    // 样式、显示距离与位置偏移：面板回填走的是同一批控件，重置当前图层后回读，
    // 值还在就说明服务（偏移则是渲染端）确实收到了写入。
    edgeLines->setChecked(true);
    offsetX->setValue(3.5);
    minDistance->setValue(12.5);
    waitFor(80);
    inspector->setCurrentLayer(View3D::LayerId {});
    inspector->setCurrentLayer(layerId);
    waitFor(50);
    if (!edgeLines->isChecked() || qAbs(minDistance->value() - 12.5) > 0.01
        || qAbs(offsetX->value() - 3.5) > 0.01)
    {
        service->removeLayer(layerId);
        return fail(QStringLiteral("样式/显示距离/偏移没有回读到（边线=%1 最近=%2 X=%3）")
                        .arg(edgeLines->isChecked() ? 1 : 0)
                        .arg(minDistance->value())
                        .arg(offsetX->value()));
    }

    // 场景树里取消勾选 → 面板必须跟随（多播回调接通）。
    item->setCheckState(0, Qt::Unchecked);
    waitFor(80);
    if (visibleCheck->isChecked())
    {
        service->removeLayer(layerId);
        return fail(QStringLiteral("树里改可见性后属性面板没跟随"));
    }
    service->removeLayer(layerId);
    waitFor(50);

    // 日志面板：AppLog 与 qWarning 都要进同一份缓冲，并且能被过滤。
    const QString appMarker =
        QStringLiteral("smoke-applog-%1").arg(QTime::currentTime().msecsSinceStartOfDay());
    const QString qtMarker =
        QStringLiteral("smoke-qwarning-%1").arg(QTime::currentTime().msecsSinceStartOfDay());
    if (auto *manager = application.getAppLogManager()) {
        if (auto *logger = manager->getLogger(QStringLiteral("smoke")))
            logger->warn(appMarker);
    }
    qWarning().noquote() << qtMarker;
    waitFor(200);
    if (!logDock->bufferContains(appMarker))
        return fail(QStringLiteral("日志面板没有收到 AppLog 记录"));
    if (!logDock->bufferContains(qtMarker))
        return fail(QStringLiteral("日志面板没有收到 Qt 告警记录"));
    const int allLines = logDock->visibleLineCount();
    logFilter->setText(appMarker);
    waitFor(50);
    const int filteredLines = logDock->visibleLineCount();
    if (filteredLines < 1 || filteredLines >= allLines)
    {
        logFilter->clear();
        return fail(QStringLiteral("日志过滤没生效（全部=%1 过滤后=%2）")
                        .arg(allLines)
                        .arg(filteredLines));
    }
    logFilter->clear();
    waitFor(50);

    if (!uiHost.actions().contains(QStringLiteral("cmd.window.properties")) ||
        !uiHost.actions().contains(QStringLiteral("cmd.window.log")))
        return fail(QStringLiteral("面板开关命令没有出现在 Ribbon 上"));

    std::cerr << "VIEW3D_INSPECTOR_LOG_OK select=1 readback=1 opacity=1 style=1 follow=1 "
              << "log=1 filter=1" << std::endl;
    return true;
}


// 3D Tiles 1.0/1.1 冒烟：清单解析、GLB/b3dm 内容转换、瓦片组上屏与卸载。
// 只依赖本机样例数据，VIEW3D_TILES_PATH 未设置或文件不存在时跳过。
bool verifyTilesetDatasetSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString manifest = qEnvironmentVariable("VIEW3D_TILES_PATH");
    if (manifest.isEmpty() || !QFileInfo::exists(manifest)) {
        std::cerr << "VIEW3D_TILES_SKIPPED reason=env_unset" << std::endl;
        return true;
    }
    auto *container = application.getPluginContainer();
    auto *loader = container
        ? container->objectAs<View3D::ITilesetLoader>(View3D::ServiceIds::TilesetLoader)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    View3D::IRenderSurface *view = layout ? layout->primaryView() : nullptr;
    if (!loader || !scene || !view)
        return fail(QStringLiteral("3D Tiles 服务、场景或视口不可用"));

    const QSize shotSize(512, 512);
    const QImage background =
        view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
    const std::uint64_t baseVertices = scene->registeredVertexCount();
    const QString groupKey =
        QStringLiteral("tileset:") + QDir::cleanPath(QFileInfo(manifest).absoluteFilePath());
    QElapsedTimer loadClock;
    loadClock.start();
    if (!loader->loadTileset(manifest))
        return fail(loader->lastError());

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 20000 &&
           (scene->tileCount(groupKey) == 0 ||
            scene->registeredVertexCount() <= baseVertices)) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint64_t added = scene->registeredVertexCount() - baseVertices;
    if (added == 0 || scene->tileCount(groupKey) == 0)
        return fail(QStringLiteral("3D Tiles 瓦片未产生可见几何：%1").arg(groupKey));
    View3D::RenderBounds bounds;
    if (!scene->tileGroupBounds(groupKey, &bounds) || !bounds.valid)
        return fail(QStringLiteral("3D Tiles 瓦片组缺少包围盒"));

    // ── 参照系：3D Tiles 固定 WGS84 地心笛卡尔（EPSG:4978），且必须回填
    //    "地心坐标→场景局部(E,U,-N)"的刚体矩阵，图层属性就展示这一份 ──────────
    double frameTranslation = 0.0;
    if (auto *layers = container->objectAs<View3D::ISceneLayerService>(
            View3D::ServiceIds::SceneLayers)) {
        const View3D::LayerId tileLayer =
            layers->layerByRenderKey(QStringLiteral("tile:") + groupKey);
        if (tileLayer.isNull())
            return fail(QStringLiteral("3D Tiles 瓦片组没有对应图层"));
        if (!layers->layerSourceCrs(tileLayer).startsWith(QLatin1String("EPSG:4978")))
            return fail(QStringLiteral("3D Tiles 图层参照系应为 EPSG:4978，实际 %1")
                            .arg(layers->layerSourceCrs(tileLayer)));
        QElapsedTimer frameClock;
        frameClock.start();
        View3D::ILayer::ReferenceFrame frame;
        while (frameClock.elapsed() < 5000) {
            frame = layers->layerReferenceFrame(tileLayer);
            if (frame.hasMatrix)
                break;
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QString problem = rigidFrameProblem(frame);
        if (!problem.isEmpty())
            return fail(QStringLiteral("3D Tiles 参照系矩阵异常：%1").arg(problem));
        // 两种合法形态：地心坐标成果的平移量 = |originEcef| ≈ 6.4e6 m；
        // 本地坐标成果（样例 1.1 MultipleContents 就是）走恒等矩阵。
        frameTranslation = std::sqrt(frame.matrix[3] * frame.matrix[3] +
                                     frame.matrix[7] * frame.matrix[7] +
                                     frame.matrix[11] * frame.matrix[11]);
        const bool identity = frameTranslation < 1e-9 &&
            qAbs(frame.matrix[0] - 1.0) < 1e-12 && qAbs(frame.matrix[5] - 1.0) < 1e-12 &&
            qAbs(frame.matrix[10] - 1.0) < 1e-12;
        if (!identity && frameTranslation < 6.0e6)
            return fail(QStringLiteral("3D Tiles 变换矩阵的平移不是地心尺度：%1")
                            .arg(frameTranslation));
        // 地心坐标成果要能标出经纬度；本地坐标成果没有地理位置，留空。
        if (!identity && frame.position.isEmpty())
            return fail(QStringLiteral("3D Tiles 图层缺少经纬度"));
        std::cerr << "VIEW3D_TILES_REF_FRAME_OK crs=EPSG:4978 translation="
                  << frameTranslation << " position=" << frame.position.toStdString()
                  << std::endl;
    }

    // 等 GPU 上传完成再抓图：addTile 只代表登记，Pending 时画面里还没有内容。
    const auto findReadyTile = [&](QString *tileKey) {
        const QString prefix = QStringLiteral("tile:") + groupKey + QLatin1Char('/');
        for (const QString &layerKey : scene->modelLayerKeys()) {
            if (!layerKey.startsWith(prefix))
                continue;
            const QString key = layerKey.mid(prefix.size());
            if (scene->tileRenderState(groupKey, key) != View3D::LayerRenderState::Ready)
                continue;
            if (!scene->isTileVisible(groupKey, key))
                continue;
            if (tileKey)
                *tileKey = key;
            return true;
        }
        return false;
    };
    timer.restart();
    QString readyKey;
    while (timer.elapsed() < 15000 && !findReadyTile(&readyKey)) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (!findReadyTile(&readyKey))
        return fail(QStringLiteral("3D Tiles 瓦片始终没有完成上屏"));
    view->focusTileGroup(groupKey);
    const int settleMs = qEnvironmentVariable("VIEW3D_TILES_SETTLE_MS").toInt() > 0
        ? qEnvironmentVariable("VIEW3D_TILES_SETTLE_MS").toInt()
        : 2000;
    timer.restart();
    while (timer.elapsed() < settleMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const QImage shot = view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
    const std::uint32_t settledTiles = scene->tileCount(groupKey);
    const std::uint64_t settledVertices = scene->registeredVertexCount() - baseVertices;
    const QString dumpPrefix = qEnvironmentVariable("VIEW3D_TILES_DUMP");
    if (!dumpPrefix.isEmpty() && !shot.isNull())
        shot.save(dumpPrefix + QStringLiteral("_tiles.png"));
    if (qEnvironmentVariableIsSet("VIEW3D_TILES_MAIN_CAPTURE")) {
        const QImage mainShot = view->captureImage(shotSize, View3D::CaptureOptions());
        const ShotStats mainStats = analyseShot(mainShot, QImage());
        if (!dumpPrefix.isEmpty() && !mainShot.isNull())
            mainShot.save(dumpPrefix + QStringLiteral("_tiles_main.png"));
        std::cerr << "VIEW3D_TILES_MAIN_CAPTURE valid=" << (mainShot.isNull() ? 0 : 1)
                  << " lum=" << mainStats.meanLum
                  << " colours=" << mainStats.distinctColours << std::endl;
    }
    const ShotStats stats = analyseShot(shot, QImage());
    int changedPixels = 0;
    if (!shot.isNull() && !background.isNull() && shot.size() == background.size()) {
        const QImage current = shot.convertToFormat(QImage::Format_ARGB32);
        const QImage base = background.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < current.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(current.constScanLine(y));
            const auto *baseLine = reinterpret_cast<const QRgb *>(base.constScanLine(y));
            for (int x = 0; x < current.width(); ++x) {
                if (qAbs(qRed(line[x]) - qRed(baseLine[x])) +
                        qAbs(qGreen(line[x]) - qGreen(baseLine[x])) +
                        qAbs(qBlue(line[x]) - qBlue(baseLine[x])) >
                    30)
                    ++changedPixels;
            }
        }
    }
    const int changedPermille =
        shot.isNull() ? 0 : changedPixels * 1000 / (shot.width() * shot.height());

    // 相机框到数据集中心后，从相机指向 target 的射线应命中本瓦片组；
    // 同时验证转换器写入的节点 worldTransform 与射线索引一致。
    bool picked = false;
    QString pickedKey;
    View3D::ViewportCameraState cameraState;
    if (view->cameraState(&cameraState)) {
        std::array<double, 3> direction = cameraState.direction;
        const std::array<double, 3> toTarget {
            cameraState.target[0] - cameraState.position[0],
            cameraState.target[1] - cameraState.position[1],
            cameraState.target[2] - cameraState.position[2]};
        const double targetLength = std::sqrt(toTarget[0] * toTarget[0] +
                                               toTarget[1] * toTarget[1] +
                                               toTarget[2] * toTarget[2]);
        if (targetLength > 1e-6) {
            direction = { toTarget[0] / targetLength, toTarget[1] / targetLength,
                          toTarget[2] / targetLength };
        }
        View3D::ViewportPickResult pick;
        if (scene->pickRay(cameraState.position, direction, &pick) &&
            pick.hasWorldPosition) {
            const QString prefix = QStringLiteral("tile:") + groupKey + QLatin1Char('/');
            picked = pick.renderKey.startsWith(prefix);
            pickedKey = pick.renderKey;
        }
    }
    if (qEnvironmentVariableIsSet("VIEW3D_TILES_PICK_STRICT") && !picked)
        return fail(QStringLiteral("3D Tiles 射线拾取未命中瓦片组：%1").arg(pickedKey));

    std::cerr << "VIEW3D_TILES_STREAM_OK group=" << groupKey.toStdString()
              << " first_ms=" << loadClock.elapsed() << " tiles=" << scene->tileCount(groupKey)
              << " vertices=" << added << " textures=" << scene->registeredTextureCount()
              << " settled_tiles=" << settledTiles
              << " settled_vertices=" << settledVertices
              << " ready=" << readyKey.toStdString()
              << " lum=" << stats.meanLum << " colours=" << stats.distinctColours
              << " changed_permille=" << changedPermille
              << " picked=" << (picked ? 1 : 0)
              << " pick_key=" << pickedKey.toStdString()
              << " min=" << bounds.minimum[0] << "," << bounds.minimum[1] << ","
              << bounds.minimum[2] << " max=" << bounds.maximum[0] << ","
              << bounds.maximum[1] << "," << bounds.maximum[2] << std::endl;
    if (changedPermille < 5)
        return fail(QStringLiteral("3D Tiles 已加载但画面没有变化：changed_permille=%1")
                        .arg(changedPermille));

    // 诊断：3D Tiles 右键旋转枢轴是否切到光标下的表面点。与 OSGB 的
    // VIEW3D_NAV_PROBE 相同，输入事件走 Renderer 的共用路径；这里只读
    // cameraState().target（即 m_cameraTarget）。
    if (qEnvironmentVariableIsSet("VIEW3D_TILES_NAV_PROBE")) {
        QWidget* navWidget = view->widget();
        View3D::ViewportCameraState navBefore;
        if (navWidget && navWidget->width() > 0 && navWidget->height() > 0 &&
            view->cameraState(&navBefore)) {
            const auto distance3 = [](const std::array<double, 3>& a,
                                      const std::array<double, 3>& b) {
                const double dx = a[0] - b[0];
                const double dy = a[1] - b[1];
                const double dz = a[2] - b[2];
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            const double navX = navWidget->width() * 0.5;
            const double navY = navWidget->height() * 0.5;
            const QPoint navGlobal = navWidget->mapToGlobal(
                QPoint(qRound(navX), qRound(navY)));
            View3D::ViewportPickResult centerPick;
            const bool centerHit = scene->pickRay(
                navBefore.position, navBefore.direction, &centerPick) &&
                centerPick.hasWorldPosition;
            QMouseEvent navPress(QEvent::MouseButtonPress, QPointF(navX, navY),
                                 QPointF(navGlobal), Qt::RightButton, Qt::RightButton,
                                 Qt::NoModifier);
            QApplication::sendEvent(navWidget, &navPress);
            View3D::ViewportCameraState navAfter;
            const bool navRead = view->cameraState(&navAfter);
            const double pivotMoved =
                navRead ? distance3(navAfter.target, navBefore.target) : -1.0;
            const bool orbited = view->orbitCamera(10.0, 0.0);
            View3D::ViewportCameraState navOrbited;
            const bool orbitRead = orbited && view->cameraState(&navOrbited);
            const double orbitDrift = orbitRead
                ? distance3(navOrbited.target, navAfter.target)
                : -1.0;
            QMouseEvent navRelease(QEvent::MouseButtonRelease, QPointF(navX, navY),
                                   QPointF(navGlobal), Qt::RightButton, Qt::NoButton,
                                   Qt::NoModifier);
            QApplication::sendEvent(navWidget, &navRelease);
            std::cerr << "VIEW3D_TILES_NAV center_hit=" << (centerHit ? 1 : 0)
                      << " pivot_moved=" << pivotMoved
                      << " orbit_drift=" << orbitDrift
                      << " pick_pos="
                      << centerPick.worldPosition[0] << ","
                      << centerPick.worldPosition[1] << ","
                      << centerPick.worldPosition[2] << std::endl;
        }
    }

    // 可选：连续推近验证 LOD 会继续下钻、旧瓦片会被替换/回收。
    const int zoomSteps = qEnvironmentVariable("VIEW3D_TILES_ZOOM_STEPS").toInt();
    if (zoomSteps > 0) {
        // 顶视模式：从包围盒中心正上方垂直向下看，再往下推近。
        // 斜视时"空白"很难区分是相机越过了数据边界还是瓦片真的缺了，顶视时每条
        // 空白都落在数据带内部，可以直接判读。
        if (qEnvironmentVariableIsSet("VIEW3D_TILES_TOPVIEW")) {
            View3D::RenderBounds topBounds;
            if (scene->tileGroupBounds(groupKey, &topBounds) && topBounds.valid) {
                View3D::ViewportCameraState top;
                if (view->cameraState(&top)) {
                    const double centerX =
                        (topBounds.minimum[0] + topBounds.maximum[0]) * 0.5;
                    const double centerY =
                        (topBounds.minimum[1] + topBounds.maximum[1]) * 0.5;
                    const double centerZ =
                        (topBounds.minimum[2] + topBounds.maximum[2]) * 0.5;
                    const double extent = std::max(
                        {topBounds.maximum[0] - topBounds.minimum[0],
                         topBounds.maximum[2] - topBounds.minimum[2]});
                    top.position = {centerX, centerY + extent * 1.6, centerZ};
                    top.target = {centerX, centerY, centerZ};
                    top.direction = {0.0, -1.0, 0.0};
                    top.up = {0.0, 0.0, -1.0};
                    view->setCameraState(top);
                    QApplication::processEvents(QEventLoop::AllEvents, 20);
                }
            }
        }
        const QSize renderSize = view->renderPixelSize();
        const int centerX = qMax(1, renderSize.width() / 2);
        const int centerY = qMax(1, renderSize.height() / 2);
        // 逐档探针：每推近一档都等一小段，统计本档登记/可见/就绪的瓦片数并抓图。
        // 与流式日志里的 selected 对照，可见数明显少就是画面上的缺口。
        const bool zoomProbe = qEnvironmentVariableIsSet("VIEW3D_TILES_ZOOM_PROBE");
        const int zoomProbeWaitMs =
            qEnvironmentVariable("VIEW3D_TILES_ZOOM_STEP_WAIT_MS").toInt() > 0
            ? qEnvironmentVariable("VIEW3D_TILES_ZOOM_STEP_WAIT_MS").toInt()
            : 1500;
        const QString tilePrefix = QStringLiteral("tile:") + groupKey + QLatin1Char('/');
        bool zoomed = false;
        for (int step = 0; step < zoomSteps; ++step) {
            zoomed = view->zoomCameraAt(centerX, centerY, 1) || zoomed;
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(15);
            if (!zoomProbe)
                continue;
            QElapsedTimer stepClock;
            stepClock.start();
            while (stepClock.elapsed() < zoomProbeWaitMs) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            std::uint32_t visibleTiles = 0;
            std::uint32_t readyTiles = 0;
            for (const QString& layerKey : scene->modelLayerKeys()) {
                if (!layerKey.startsWith(tilePrefix))
                    continue;
                const QString key = layerKey.mid(tilePrefix.size());
                if (scene->isTileVisible(groupKey, key))
                    ++visibleTiles;
                if (scene->tileRenderState(groupKey, key) == View3D::LayerRenderState::Ready)
                    ++readyTiles;
            }
            const QImage stepShot =
                view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
            if (!dumpPrefix.isEmpty() && !stepShot.isNull())
                stepShot.save(dumpPrefix + QStringLiteral("_zoom_step%1.png").arg(step + 1));
            View3D::ViewportCameraState stepCamera;
            const bool haveStepCamera = view->cameraState(&stepCamera);
            const double cameraDistance = haveStepCamera
                ? std::sqrt(std::pow(stepCamera.position[0] - stepCamera.target[0], 2.0) +
                            std::pow(stepCamera.position[1] - stepCamera.target[1], 2.0) +
                            std::pow(stepCamera.position[2] - stepCamera.target[2], 2.0))
                : -1.0;
            View3D::RenderBounds stepBounds;
            const bool haveStepBounds = scene->tileGroupBounds(groupKey, &stepBounds);
            std::cerr << "VIEW3D_TILES_ZOOM_PROBE step=" << (step + 1)
                      << " distance=" << cameraDistance
                      << " resident=" << scene->tileCount(groupKey)
                      << " visible=" << visibleTiles << " ready=" << readyTiles
                      << " vertices=" << (scene->registeredVertexCount() - baseVertices);
            if (haveStepCamera)
                std::cerr << " cam=" << stepCamera.position[0] << ","
                          << stepCamera.position[1] << "," << stepCamera.position[2]
                          << " target=" << stepCamera.target[0] << ","
                          << stepCamera.target[1] << "," << stepCamera.target[2];
            if (haveStepBounds)
                std::cerr << " bounds_y=" << stepBounds.minimum[1] << ","
                          << stepBounds.maximum[1];
            std::cerr << std::endl;
        }
        const int zoomSettleMs = qEnvironmentVariable("VIEW3D_TILES_ZOOM_SETTLE_MS").toInt() > 0
            ? qEnvironmentVariable("VIEW3D_TILES_ZOOM_SETTLE_MS").toInt()
            : 8000;
        timer.restart();
        while (timer.elapsed() < zoomSettleMs) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const std::uint32_t zoomTiles = scene->tileCount(groupKey);
        const std::uint64_t zoomVertices = scene->registeredVertexCount() - baseVertices;
        const QImage zoomShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        const ShotStats zoomStats = analyseShot(zoomShot, QImage());
        if (!dumpPrefix.isEmpty() && !zoomShot.isNull())
            zoomShot.save(dumpPrefix + QStringLiteral("_tiles_zoom.png"));
        std::cerr << "VIEW3D_TILES_ZOOM steps=" << zoomSteps << " zoomed=" << (zoomed ? 1 : 0)
                  << " tiles=" << zoomTiles << " vertices=" << zoomVertices
                  << " lum=" << zoomStats.meanLum
                  << " colours=" << zoomStats.distinctColours << std::endl;
        if (!zoomed)
            return fail(QStringLiteral("3D Tiles 推近相机失败"));
        if (zoomTiles == settledTiles && zoomVertices == settledVertices)
            return fail(QStringLiteral("3D Tiles 推近后瓦片选择没有变化"));

        const int zoomOutSteps = qEnvironmentVariable("VIEW3D_TILES_ZOOM_OUT_STEPS").toInt();
        if (zoomOutSteps > 0) {
            for (int step = 0; step < zoomOutSteps; ++step) {
                view->zoomCameraAt(centerX, centerY, -1);
                QApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(15);
            }
            timer.restart();
            while (timer.elapsed() < zoomSettleMs) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            const QImage zoomOutShot =
                view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
            const ShotStats zoomOutStats = analyseShot(zoomOutShot, QImage());
            std::cerr << "VIEW3D_TILES_ZOOM_OUT steps=" << zoomOutSteps
                      << " tiles=" << scene->tileCount(groupKey)
                      << " vertices=" << (scene->registeredVertexCount() - baseVertices)
                      << " lum=" << zoomOutStats.meanLum
                      << " colours=" << zoomOutStats.distinctColours << std::endl;
        }
    }

    // ── 球面模式：在真实 ECEF 数据上进入/退出各一次。验证相机局部↔ECEF 往返、
    //    会话重启后按球面重新烘焙上屏、椭球底面出图、非瓦片图层隐藏/恢复、
    //    静态画面抖动。VIEW3D_TILES_GLOBE 未设置时跳过 ───────────────────────
    if (qEnvironmentVariableIsSet("VIEW3D_TILES_GLOBE")) {
        auto *reference = container->objectAs<View3D::IGeoReference>(
            View3D::ServiceIds::GeoReference);
        auto *layers = container->objectAs<View3D::ISceneLayerService>(
            View3D::ServiceIds::SceneLayers);
        if (!reference || !reference->ready())
            return fail(QStringLiteral("球面模式需要已就绪的地理参考"));
        const View3D::LayerId tileLayer = layers
            ? layers->layerByRenderKey(QStringLiteral("tile:") + groupKey)
            : View3D::LayerId {};
        if (!tileLayer.isNull() && !layers->layerReferenceFrame(tileLayer).hasMatrix)
            return fail(QStringLiteral("平面模式下图层参照系应展示局部换算矩阵"));
        const auto dist3 = [](const std::array<double, 3> &a,
                              const std::array<double, 3> &b) {
            const double dx = a[0] - b[0];
            const double dy = a[1] - b[1];
            const double dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const auto waitTiles = [&](int timeoutMs) {
            timer.restart();
            while (timer.elapsed() < timeoutMs && scene->tileCount(groupKey) == 0) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            return scene->tileCount(groupKey) > 0;
        };
        const auto pixelDiffPermille = [](const QImage &a, const QImage &b) {
            if (a.isNull() || b.isNull() || a.size() != b.size())
                return -1;
            const QImage left = a.convertToFormat(QImage::Format_ARGB32);
            const QImage right = b.convertToFormat(QImage::Format_ARGB32);
            int changed = 0;
            for (int y = 0; y < left.height(); ++y) {
                const auto *leftLine =
                    reinterpret_cast<const QRgb *>(left.constScanLine(y));
                const auto *rightLine =
                    reinterpret_cast<const QRgb *>(right.constScanLine(y));
                for (int x = 0; x < left.width(); ++x) {
                    if (qAbs(qRed(leftLine[x]) - qRed(rightLine[x])) +
                            qAbs(qGreen(leftLine[x]) - qGreen(rightLine[x])) +
                            qAbs(qBlue(leftLine[x]) - qBlue(rightLine[x])) >
                        30)
                        ++changed;
                }
            }
            return changed * 1000 / (left.width() * left.height());
        };

        // 平面覆盖层（影像）探针：球面下必须隐藏，退出后恢复。模型与瓦片在球面
        // 下由数据插件重烘焙，保持可见，不再用模型做隐藏探针。影像层没有渲染
        // 几何，隐藏与否看场景树的勾选状态（用户看到的就是这一份）。
        View3D::LayerId probeLayer;
        QTreeWidget *probeTree = nullptr;
        if (layers) {
            probeLayer = layers->addImageryLayer(QStringLiteral("球面隐藏探针"),
                                                 QStringLiteral("globe_hide_probe"));
            if (probeLayer.isNull())
                return fail(QStringLiteral("球面探针图层登记失败"));
            auto *dock = container->objectAs<View3D::ISceneTreeDock>(
                View3D::ServiceIds::SceneTreeDock);
            probeTree = dock ? dock->sceneTree() : nullptr;
        }
        const auto probeState = [probeTree]() -> int {
            if (!probeTree)
                return -1;
            for (QTreeWidgetItemIterator it(probeTree); *it; ++it) {
                if ((*it)->text(0).contains(QStringLiteral("球面隐藏探针")))
                    return static_cast<int>((*it)->checkState(0));
            }
            return -1;
        };
        const auto waitProbeState = [&probeState](Qt::CheckState state, int timeoutMs) {
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < timeoutMs) {
                if (probeState() == static_cast<int>(state))
                    return true;
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            return probeState() == static_cast<int>(state);
        };
        if (probeTree && !waitProbeState(Qt::Checked, 500))
            return fail(QStringLiteral("球面探针图层初始不可见"));

        View3D::ViewportCameraState flatBefore;
        if (!view->cameraState(&flatBefore))
            return fail(QStringLiteral("球面切换前无法读取相机"));
        std::array<double, 3> expectedEcef {};
        if (!reference->localToEcefPoint(flatBefore.position, &expectedEcef))
            return fail(QStringLiteral("相机局部坐标换算 ECEF 失败"));
        const std::uint64_t flatVertices = scene->registeredVertexCount() - baseVertices;

        if (!layout->setGlobeModeEnabled(true))
            return fail(QStringLiteral("进入球面模式失败"));
        if (!scene->globeModeEnabled() || !view->globeModeEnabled())
            return fail(QStringLiteral("球面模式没有传播到场景/视口"));
        if (probeTree && !waitProbeState(Qt::Unchecked, 3000))
            return fail(QStringLiteral("球面模式下影像图层没有隐藏"));
        // 球面直接按 ECEF 摆放，不再乘"地心坐标→场景局部"矩阵，面板也不该展示它。
        if (!tileLayer.isNull() && layers->layerReferenceFrame(tileLayer).hasMatrix)
            return fail(QStringLiteral("球面模式下图层参照系仍展示局部换算矩阵"));

        View3D::ViewportCameraState globeCamera;
        if (!view->cameraState(&globeCamera))
            return fail(QStringLiteral("球面模式下无法读取相机"));
        const double cameraRadius = std::sqrt(
            globeCamera.position[0] * globeCamera.position[0] +
            globeCamera.position[1] * globeCamera.position[1] +
            globeCamera.position[2] * globeCamera.position[2]);
        if (cameraRadius < 6.0e6 || cameraRadius > 1.0e9)
            return fail(QStringLiteral("球面相机不在 ECEF 尺度：|p|=%1").arg(cameraRadius));
        const double enterError = dist3(globeCamera.position, expectedEcef);
        if (enterError > 1.0)
            return fail(QStringLiteral("进入球面的相机换算误差过大：%1 m").arg(enterError));
        std::array<double, 3> anchor {};
        if (!scene->globeRenderOrigin(&anchor))
            return fail(QStringLiteral("球面模式没有渲染原点"));

        if (!waitTiles(30000))
            return fail(QStringLiteral("球面模式下 3D Tiles 没有重新上屏"));
        // 等瓦片选择稳定（连续 1 s 数量不变）再抓图，避免把 LOD 精化计成抖动。
        int stableTiles = -1;
        timer.restart();
        qint64 stableSince = 0;
        while (timer.elapsed() < 15000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
            const int count = static_cast<int>(scene->tileCount(groupKey));
            if (count != stableTiles) {
                stableTiles = count;
                stableSince = timer.elapsed();
            } else if (timer.elapsed() - stableSince > 1000) {
                break;
            }
        }
        const QImage globeShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (!dumpPrefix.isEmpty() && !globeShot.isNull())
            globeShot.save(dumpPrefix + QStringLiteral("_globe.png"));
        const ShotStats globeStats = analyseShot(globeShot, QImage());
        int globeChangedPixels = 0;
        if (!globeShot.isNull() && !background.isNull() &&
            globeShot.size() == background.size()) {
            const QImage current = globeShot.convertToFormat(QImage::Format_ARGB32);
            const QImage base = background.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < current.height(); ++y) {
                const auto *line = reinterpret_cast<const QRgb *>(current.constScanLine(y));
                const auto *baseLine =
                    reinterpret_cast<const QRgb *>(base.constScanLine(y));
                for (int x = 0; x < current.width(); ++x) {
                    if (qAbs(qRed(line[x]) - qRed(baseLine[x])) +
                            qAbs(qGreen(line[x]) - qGreen(baseLine[x])) +
                            qAbs(qBlue(line[x]) - qBlue(baseLine[x])) >
                        30)
                        ++globeChangedPixels;
                }
            }
        }
        const int globeChangedPermille = globeShot.isNull()
            ? 0
            : globeChangedPixels * 1000 / (globeShot.width() * globeShot.height());
        if (globeChangedPermille < 5)
            return fail(QStringLiteral("球面模式下画面没有内容：changed_permille=%1")
                            .arg(globeChangedPermille));
        // 相机不动、瓦片稳定时两帧应几乎一致：差值即球面浮点抖动的上界。
        timer.restart();
        while (timer.elapsed() < 400) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QImage globeShot2 =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        const int jitterPermille = pixelDiffPermille(globeShot, globeShot2);
        if (jitterPermille < 0)
            return fail(QStringLiteral("球面静态帧抓图失败"));
        if (jitterPermille > 10)
            return fail(QStringLiteral("球面静态画面抖动过大：%1‰").arg(jitterPermille));
        const std::uint64_t globeVertices = scene->registeredVertexCount() - baseVertices;

        // 渲染锚点必须跟随活动视口相机：两者不一致时瓦片会按旧锚点绘制（画面跳）。
        std::array<double, 3> finalAnchor {};
        View3D::ViewportCameraState anchorCamera;
        if (!scene->globeRenderOrigin(&finalAnchor) || !view->cameraState(&anchorCamera))
            return fail(QStringLiteral("球面锚点或相机不可读"));
        const double anchorFollowError = dist3(finalAnchor, anchorCamera.position);
        if (anchorFollowError > 1.0)
            return fail(QStringLiteral("球面渲染锚点没有跟随相机：%1 m")
                            .arg(anchorFollowError));

        // 球面轨道旋转应可用（yaw 15° 绕目标点）。
        View3D::ViewportCameraState orbitStart;
        view->cameraState(&orbitStart);
        const bool orbited = view->orbitCamera(15.0, 0.0);
        View3D::ViewportCameraState orbitEnd;
        view->cameraState(&orbitEnd);
        const double orbitDot = std::clamp(
            orbitStart.direction[0] * orbitEnd.direction[0] +
                orbitStart.direction[1] * orbitEnd.direction[1] +
                orbitStart.direction[2] * orbitEnd.direction[2],
            -1.0, 1.0);
        const double orbitTurn = std::acos(orbitDot) * 180.0 / 3.141592653589793;
        if (!orbited || orbitTurn < 1.0)
            return fail(QStringLiteral("球面轨道旋转不可用：turn=%1°").arg(orbitTurn));

        // 球面测量叠加层：按 ECEF 提交两个点，转动相机（锚点变化）后仍要可见。
        std::array<double, 3> overlayAnchor {};
        if (!scene->globeRenderOrigin(&overlayAnchor))
            return fail(QStringLiteral("球面测量验证缺少渲染原点"));
        View3D::ViewportPickResult overlayPick;
        const std::array<double, 3> overlayRayOrigin = {
            orbitEnd.position[0] - overlayAnchor[0],
            orbitEnd.position[1] - overlayAnchor[1],
            orbitEnd.position[2] - overlayAnchor[2]};
        std::array<double, 3> overlayA = orbitEnd.target;
        if (scene->pickRay(overlayRayOrigin, orbitEnd.direction, &overlayPick) &&
            overlayPick.hasWorldPosition) {
            overlayA = { overlayPick.worldPosition[0] + overlayAnchor[0],
                         overlayPick.worldPosition[1] + overlayAnchor[1],
                         overlayPick.worldPosition[2] + overlayAnchor[2] };
        }
        const View3D::GlobeMath::Vec3 overlayUp = View3D::GlobeMath::ecefNormal(overlayA);
        View3D::GlobeMath::Vec3 overlayEast = View3D::GlobeMath::normalized(
            View3D::GlobeMath::cross({ 0.0, 0.0, 1.0 }, overlayUp));
        if (View3D::GlobeMath::length(overlayEast) < 1e-6)
            overlayEast = { 1.0, 0.0, 0.0 };
        const std::array<double, 3> overlayB = View3D::GlobeMath::add(
            overlayA, View3D::GlobeMath::scale(overlayEast, 30.0));
        if (!view->setOverlay({ overlayA, overlayB }, true))
            return fail(QStringLiteral("球面测量叠加层提交失败"));
        View3D::CaptureOptions overlayCapture;
        overlayCapture.includeMeasurementOverlay = true;
        const auto countOverlayPixels = [](const QImage &image) {
            if (image.isNull())
                return 0;
            int count = 0;
            const QImage rgb = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < rgb.height(); ++y) {
                const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
                for (int x = 0; x < rgb.width(); ++x) {
                    if (qRed(line[x]) > 220 && qGreen(line[x]) > 120 &&
                        qGreen(line[x]) < 215 && qBlue(line[x]) < 70)
                        ++count;
                }
            }
            return count;
        };
        const int overlayBefore =
            countOverlayPixels(view->captureImageOffscreen(shotSize, overlayCapture));
        if (!view->orbitCamera(-25.0, 0.0))
            return fail(QStringLiteral("球面测量验证的相机转动失败"));
        timer.restart();
        while (timer.elapsed() < 400) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QImage overlayShot =
            view->captureImageOffscreen(shotSize, overlayCapture);
        if (!dumpPrefix.isEmpty() && !overlayShot.isNull())
            overlayShot.save(dumpPrefix + QStringLiteral("_globe_measure.png"));
        const int overlayAfter = countOverlayPixels(overlayShot);
        view->clearOverlay();
        if (overlayBefore < 20 || overlayAfter < 20)
            return fail(QStringLiteral("球面测量叠加层不可见：before=%1 after=%2")
                            .arg(overlayBefore)
                            .arg(overlayAfter));
        view->setCameraState(orbitEnd);

        // 球面缩放拉远：高度必须按滚轮累积放大，并能看到椭球面（远景 dump）。
        for (int step = 0; step < 40; ++step) {
            view->zoomCamera(-1.0);
            QApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        View3D::ViewportCameraState farCamera;
        if (!view->cameraState(&farCamera))
            return fail(QStringLiteral("球面拉远后无法读取相机"));
        const double farAltitude = View3D::GlobeMath::ecefHeight(farCamera.position);
        if (farAltitude < 1.0e5)
            return fail(QStringLiteral("球面拉远后高度过低：%1 m").arg(farAltitude));
        timer.restart();
        while (timer.elapsed() < 800) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        // 球面底图替换通路（在线影像走的就是它）：换纯色立即生效，再换回内置图。
        {
            std::vector<unsigned char> magenta(512u * 256u * 4u);
            for (std::size_t i = 0; i < magenta.size(); i += 4) {
                magenta[i] = 255;
                magenta[i + 1] = 0;
                magenta[i + 2] = 255;
                magenta[i + 3] = 255;
            }
            if (!scene->setGlobeBasemapImage(512, 256, magenta))
                return fail(QStringLiteral("球面底图替换失败"));
            timer.restart();
            while (timer.elapsed() < 500) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            const QImage swapped =
                view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
            int magentaPixels = 0;
            if (!swapped.isNull()) {
                const QImage rgb = swapped.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < rgb.height(); ++y) {
                    const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
                    for (int x = 0; x < rgb.width(); ++x) {
                        if (qRed(line[x]) > 220 && qGreen(line[x]) < 60 &&
                            qBlue(line[x]) > 220)
                            ++magentaPixels;
                    }
                }
            }
            // 换回内置静态底图，后面的色彩/覆盖判据仍按部署的底图判读。
            const QImage original(QCoreApplication::applicationDirPath() +
                                  QStringLiteral("/resources/globe_basemap.jpg"));
            if (original.isNull())
                return fail(QStringLiteral("找不到内置球面底图"));
            const QImage rgbaImage = original.convertToFormat(QImage::Format_RGBA8888);
            std::vector<unsigned char> restore(
                static_cast<std::size_t>(rgbaImage.width()) * rgbaImage.height() * 4u);
            for (int y = 0; y < rgbaImage.height(); ++y) {
                std::memcpy(restore.data() +
                                static_cast<std::size_t>(y) * rgbaImage.width() * 4u,
                            rgbaImage.constScanLine(y),
                            static_cast<std::size_t>(rgbaImage.width()) * 4u);
            }
            if (!scene->setGlobeBasemapImage(rgbaImage.width(), rgbaImage.height(),
                                             restore))
                return fail(QStringLiteral("恢复内置球面底图失败"));
            timer.restart();
            while (timer.elapsed() < 500) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            if (magentaPixels < 100)
                return fail(QStringLiteral("球面底图替换未生效：pixels=%1")
                                .arg(magentaPixels));
        }
        const QImage farShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (!dumpPrefix.isEmpty() && !farShot.isNull())
            farShot.save(dumpPrefix + QStringLiteral("_globe_far.png"));
        // 默认底图：远景整球应出现大量不同颜色（纯色球面只有个位数）。
        const ShotStats farStats = analyseShot(farShot, QImage());
        if (farStats.distinctColours < 30)
            return fail(QStringLiteral("球面默认底图未上屏：colours=%1")
                            .arg(farStats.distinctColours));
        // 同一机位朝天看只有天空渐变：与远景不同的像素就是椭球面覆盖率，
        // 不依赖底面具体颜色。
        View3D::ViewportCameraState skyCamera = farCamera;
        const View3D::GlobeMath::Vec3 farNormal =
            View3D::GlobeMath::ecefNormal(farCamera.position);
        View3D::GlobeMath::Vec3 farEast = View3D::GlobeMath::normalized(
            View3D::GlobeMath::cross({0.0, 0.0, 1.0}, farNormal));
        if (View3D::GlobeMath::length(farEast) < 1e-6)
            farEast = {1.0, 0.0, 0.0};
        skyCamera.target = View3D::GlobeMath::add(farCamera.position, farNormal);
        skyCamera.direction = farNormal;
        skyCamera.up = farEast;
        view->setCameraState(skyCamera);
        timer.restart();
        while (timer.elapsed() < 400) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QImage skyShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (!dumpPrefix.isEmpty() && !skyShot.isNull())
            skyShot.save(dumpPrefix + QStringLiteral("_globe_sky.png"));
        const int earthCoverPermille = pixelDiffPermille(farShot, skyShot);
        // 底图海洋色与天空渐变接近，差异像素只占一半左右；阈值只挡"完全没有球面"。
        if (earthCoverPermille < 200)
            return fail(QStringLiteral("球面远景椭球面覆盖不足：%1‰")
                            .arg(earthCoverPermille));
        // 拉远只为验证缩放与椭球底图；退出前回到数据集附近，平面重烘焙才看得到瓦片。
        view->setCameraState(orbitEnd);

        // 退出：相机回局部切平面、图层恢复、瓦片回平面烘焙。
        std::array<double, 3> expectedLocal {};
        const bool localConverted =
            reference->ecefToLocalPoint(orbitEnd.position, &expectedLocal);
        if (!layout->setGlobeModeEnabled(false))
            return fail(QStringLiteral("退出球面模式失败"));
        if (scene->globeModeEnabled() || view->globeModeEnabled())
            return fail(QStringLiteral("退出球面后模式标志仍为真"));
        if (probeTree && !waitProbeState(Qt::Checked, 3000))
            return fail(QStringLiteral("退出球面后影像图层没有恢复可见"));
        if (!tileLayer.isNull() && !layers->layerReferenceFrame(tileLayer).hasMatrix)
            return fail(QStringLiteral("退出球面后图层参照系的换算矩阵没有恢复"));
        View3D::ViewportCameraState flatAfter;
        if (!view->cameraState(&flatAfter))
            return fail(QStringLiteral("退出球面后无法读取相机"));
        const double exitError =
            localConverted ? dist3(flatAfter.position, expectedLocal) : -1.0;
        if (!localConverted || exitError > 1.0)
            return fail(QStringLiteral("退出球面的相机换算误差过大：%1 m").arg(exitError));
        if (!waitTiles(30000))
            return fail(QStringLiteral("退出球面后 3D Tiles 没有回到平面烘焙"));
        if (!probeLayer.isNull() && layers)
            layers->removeLayer(probeLayer);

        // 单次写入：流式线程的 stderr 日志会与冒烟标记交错，拆行会让 ctest 正则漏配。
        const QString summary = QStringLiteral(
            "VIEW3D_TILES_GLOBE_OK enter_err=%1 exit_err=%2 cam_radius=%3 "
            "anchor_follow_err=%4 orbit_turn=%5 far_altitude=%6 "
            "earth_cover_permille=%7 far_colours=%8 tiles=%9 vertices_flat=%10 "
            "vertices_globe=%11 lum=%12 colours=%13 changed_permille=%14 "
            "jitter_permille=%15 probe_hidden=%16 probe_restored=%17")
                                    .arg(enterError)
                                    .arg(exitError)
                                    .arg(cameraRadius)
                                    .arg(anchorFollowError)
                                    .arg(orbitTurn)
                                    .arg(farAltitude)
                                    .arg(earthCoverPermille)
                                    .arg(farStats.distinctColours)
                                    .arg(scene->tileCount(groupKey))
                                    .arg(flatVertices)
                                    .arg(globeVertices)
                                    .arg(globeStats.meanLum)
                                    .arg(globeStats.distinctColours)
                                    .arg(globeChangedPermille)
                                    .arg(jitterPermille)
                                    .arg(probeTree ? 1 : 0)
                                    .arg(probeTree ? 1 : 0);
        std::cerr << summary.toStdString() << std::endl;
    }

    if (!loader->unloadTileset(manifest))
        return fail(loader->lastError());
    timer.restart();
    while (timer.elapsed() < 5000 && scene->tileCount(groupKey) != 0) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->tileCount(groupKey) != 0)
        return fail(QStringLiteral("卸载 3D Tiles 后瓦片组仍残留"));
    if (scene->registeredVertexCount() > baseVertices)
        return fail(QStringLiteral("卸载 3D Tiles 后顶点未释放"));

    // 崩溃压测：加载后在不同阶段立即卸载，反复走 Cesium 的异步取消/
    // 资源释放路径。VIEW3D_TILES_CHURN 未设置时跳过。
    const int churn = qEnvironmentVariable("VIEW3D_TILES_CHURN").toInt();
    if (churn > 0) {
        for (int round = 0; round < churn; ++round) {
            if (!loader->loadTileset(manifest))
                return fail(QStringLiteral("压测第 %1 轮加载失败：%2")
                                .arg(round + 1)
                                .arg(loader->lastError()));
            const int waitMs = 20 + (round * 37) % 350;
            QElapsedTimer waitClock;
            waitClock.start();
            while (waitClock.elapsed() < waitMs) {
                QApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
            if (!loader->unloadTileset(manifest))
                return fail(QStringLiteral("压测第 %1 轮卸载失败：%2")
                                .arg(round + 1)
                                .arg(loader->lastError()));
            waitClock.restart();
            while (waitClock.elapsed() < 30) {
                QApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
        }
        std::cerr << "VIEW3D_TILES_CHURN_OK rounds=" << churn << std::endl;
    }

    // 序列诊断：先卸载 A 再加载 B，检查 B 的局部包围盒/机位是否与只加载 B 一致。
    const QString secondManifest = qEnvironmentVariable("VIEW3D_TILES_PATH2");
    if (!secondManifest.isEmpty() && QFileInfo::exists(secondManifest)) {
        const std::uint64_t secondBase = scene->registeredVertexCount();
        if (!loader->loadTileset(secondManifest))
            return fail(QStringLiteral("第二份数据加载失败：%1").arg(loader->lastError()));
        const QString secondGroup = QStringLiteral("tileset:") +
            QDir::cleanPath(QFileInfo(secondManifest).absoluteFilePath());
        timer.restart();
        while (timer.elapsed() < 20000 &&
               (scene->tileCount(secondGroup) == 0 ||
                scene->registeredVertexCount() <= secondBase)) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        timer.restart();
        while (timer.elapsed() < 8000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        view->focusTileGroup(secondGroup);
        timer.restart();
        while (timer.elapsed() < 1200) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        View3D::RenderBounds secondBounds;
        scene->tileGroupBounds(secondGroup, &secondBounds);
        View3D::ViewportCameraState secondCamera;
        view->cameraState(&secondCamera);
        const auto dist3 = [](const std::array<double, 3>& a,
                              const std::array<double, 3>& b) {
            const double dx = a[0] - b[0];
            const double dy = a[1] - b[1];
            const double dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        int orbitBlocked = 0;
        double orbitTurnMax = 0.0;
        View3D::ViewportCameraState orbitBefore = secondCamera;
        for (int step = 0; step < 24; ++step) {
            View3D::ViewportCameraState orbitAfter;
            if (!view->orbitCamera(15.0, 0.0) || !view->cameraState(&orbitAfter)) {
                ++orbitBlocked;
                continue;
            }
            const double dot = std::clamp(
                orbitBefore.direction[0] * orbitAfter.direction[0] +
                    orbitBefore.direction[1] * orbitAfter.direction[1] +
                    orbitBefore.direction[2] * orbitAfter.direction[2],
                -1.0, 1.0);
            orbitTurnMax = std::max(orbitTurnMax,
                                    std::acos(dot) * 180.0 / 3.141592653589793);
            orbitBefore = orbitAfter;
        }
        const QSize renderSize = view->renderPixelSize();
        for (int step = 0; step < 20; ++step) {
            view->zoomCameraAt(qMax(1, renderSize.width() / 2),
                               qMax(1, renderSize.height() / 2), 1.0);
        }
        View3D::ViewportCameraState zoomedCamera;
        view->cameraState(&zoomedCamera);
        const double zoomDistance = dist3(zoomedCamera.position, zoomedCamera.target);
        std::cerr << "VIEW3D_TILES_SEQUENCE second=" << secondManifest.toStdString()
                  << " tiles=" << scene->tileCount(secondGroup)
                  << " vertices=" << (scene->registeredVertexCount() - secondBase)
                  << " bounds_min=" << secondBounds.minimum[0] << ","
                  << secondBounds.minimum[1] << "," << secondBounds.minimum[2]
                  << " bounds_max=" << secondBounds.maximum[0] << ","
                  << secondBounds.maximum[1] << "," << secondBounds.maximum[2]
                  << " cam_pos=" << secondCamera.position[0] << ","
                  << secondCamera.position[1] << "," << secondCamera.position[2]
                  << " cam_target=" << secondCamera.target[0] << ","
                  << secondCamera.target[1] << "," << secondCamera.target[2]
                  << " target_to_group_center="
                  << dist3(secondCamera.target,
                           {(secondBounds.minimum[0] + secondBounds.maximum[0]) * 0.5,
                            (secondBounds.minimum[1] + secondBounds.maximum[1]) * 0.5,
                            (secondBounds.minimum[2] + secondBounds.maximum[2]) * 0.5})
                  << " orbit_blocked=" << orbitBlocked
                  << " orbit_turn_max=" << orbitTurnMax
                  << " zoom_dist=" << zoomDistance << std::endl;
        if (!secondBounds.valid || scene->tileCount(secondGroup) == 0)
            return fail(QStringLiteral("第二份数据没有可见瓦片"));
        if (qEnvironmentVariableIsSet("VIEW3D_TILES_SEQUENCE_LOCAL")) {
            const double extent = std::max(
                {std::abs(secondBounds.minimum[0]), std::abs(secondBounds.minimum[1]),
                 std::abs(secondBounds.minimum[2]), std::abs(secondBounds.maximum[0]),
                 std::abs(secondBounds.maximum[1]), std::abs(secondBounds.maximum[2])});
            if (extent > 100.0)
                return fail(QStringLiteral("第二份数据仍沿用旧世界系：extent=%1")
                                .arg(extent));
        }
        if (orbitBlocked > 0)
            return fail(QStringLiteral("第二份数据轨道旋转被提前阻塞：%1 次")
                            .arg(orbitBlocked));
        if (zoomDistance > 2.0)
            return fail(QStringLiteral("第二份数据滚轮缩放无法推进：%1 m")
                            .arg(zoomDistance));
        if (!loader->unloadTileset(secondManifest))
            return fail(loader->lastError());
    }

    std::cerr << "VIEW3D_TILES_SMOKE_OK group=" << groupKey.toStdString()
              << " changed_permille=" << changedPermille << std::endl;
    return true;
}

// 真实 223 MB 3DS 厂区模型的回归。这份数据由 Lib3DS 导出：keyframer 里只有逐节点
// 块、没有父子层级记录，Assimp 于是把 9.8 万个节点挂成一条上万深的链，沿父链递归
// 直接把导入线程的 1 MB 栈打爆——进程静默闪退，日志停在 VIEW3D_MODEL_SUBMIT。
// 这里按用户真正走的入口（模型打开器）再读一遍，验证摘掉 keyframer 之后出得来、
// 量级对得上，并把耗时与内存记下来供以后比较。未设 VIEW3D_3DS_FILE 时跳过。
bool verifyThreeDsDatasetSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString path = qEnvironmentVariable("VIEW3D_3DS_FILE");
    if (path.isEmpty()) {
        std::cerr << "VIEW3D_3DS_DATASET_SKIPPED reason=env_unset" << std::endl;
        return true;
    }
    if (!QFileInfo::exists(path))
        return fail(QStringLiteral("3DS 数据不存在：%1").arg(path));

    auto *container = application.getPluginContainer();
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *openRegistry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    if (!scene || !layers || !openRegistry)
        return fail(QStringLiteral("3DS 回归缺少场景或图层服务"));

    View3D::IDataOpener *modelOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (opener && opener->displayName().contains(QStringLiteral("模型文件"))) {
            modelOpener = opener;
            break;
        }
    }
    if (!modelOpener)
        return fail(QStringLiteral("没有模型打开器，assimp 读取器没编进来？"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    // 导入选项对话框是模态的：无头跑必须在下一轮事件循环里替用户点确定。
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isModal() && dialog->isVisible() &&
                dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                dialog->accept();
                return;
            }
        }
    });
    const auto findMessageBox = [](const QString &title) -> QMessageBox * {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (box && box->isVisible() && box->windowTitle().contains(title))
                return box;
        }
        return nullptr;
    };
    QSet<QString> beforeKeys;
    for (const QString &key : scene->modelLayerKeys())
        beforeKeys.insert(key);
    const std::uint64_t baseVertices = scene->registeredVertexCount();

    const View3D::LayerId group = layers->addGroup(QStringLiteral("3DS 回归组"));
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    View3D::IRenderSurface *view = layout ? layout->primaryView() : nullptr;
    const QSize shotSize(512, 512);
    const QImage background =
        view ? view->captureImageOffscreen(shotSize, View3D::CaptureOptions()) : QImage();
    QString openError;
    if (!modelOpener->openData(path, group, &openError))
        return fail(QStringLiteral("模型打开器拒绝该文件：%1").arg(openError));

    const int waitMs = qEnvironmentVariableIntValue("VIEW3D_3DS_WAIT_MS") > 0
        ? qEnvironmentVariableIntValue("VIEW3D_3DS_WAIT_MS")
        : 600000;
    QElapsedTimer clock;
    clock.start();
    QString layerKey;
    while (clock.elapsed() < waitMs && layerKey.isEmpty()) {
        waitFor(200);
        for (const QString &key : scene->modelLayerKeys()) {
            if (!beforeKeys.contains(key)) {
                layerKey = key;
                break;
            }
        }
        if (!layerKey.isEmpty())
            break;
        if (QMessageBox *box = findMessageBox(QStringLiteral("打开数据失败"))) {
            const QString detail = box->text() + QLatin1Char(' ') + box->informativeText();
            box->done(QMessageBox::Ok);
            return fail(QStringLiteral("3DS 导入失败：%1").arg(detail.trimmed()));
        }
    }
    if (layerKey.isEmpty())
        return fail(QStringLiteral("等待 %1 秒仍没有出图层").arg(waitMs / 1000));

    const std::shared_ptr<const View3D::ModelMeshDto> mesh = scene->modelLayerMesh(layerKey);
    if (!mesh || mesh->vertices.empty() || mesh->primitives.empty())
        return fail(QStringLiteral("3DS 图层没有可用几何"));
    // 量级判据只卡下限：assimp 的后处理会合并/去重，具体数字随版本浮动，
    // 但千万级顶点、万级基元这份数据跑不出别的结果。
    if (mesh->vertices.size() < 1000000)
        return fail(QStringLiteral("3DS 顶点量级不对：%1").arg(mesh->vertices.size()));
    if (mesh->primitives.size() < 10000)
        return fail(QStringLiteral("3DS 基元数量不对：%1").arg(mesh->primitives.size()));

    const qint64 elapsedMs = clock.elapsed();
    const std::uint64_t cpuBytes = View3D::modelMeshCpuBytes(*mesh);
    const std::size_t primitiveCount = mesh->primitives.size();
    const std::size_t nodeCount = mesh->nodes.size();
    const std::uint64_t vertexCount = static_cast<std::uint64_t>(mesh->vertices.size());

    // 形状构成：按基元量三点跨度，最薄边 <= 最长边的 2% 记为"薄片"。
    // 用来回答"这数据到底是方块还是一张皮"，不靠肉眼猜。
    std::uint64_t sheetPrimitives = 0, solidPrimitives = 0, emptyPrimitives = 0;
    double longestSpan = 0.0;
    for (const View3D::ModelPrimitiveDto &primitive : mesh->primitives) {
        if (primitive.firstVertex > mesh->vertices.size() ||
            primitive.vertexCount > mesh->vertices.size() - primitive.firstVertex)
            continue;
        double mn[3] = {1e300, 1e300, 1e300};
        double mx[3] = {-1e300, -1e300, -1e300};
        for (std::uint32_t i = 0; i < primitive.vertexCount; ++i) {
            const View3D::MeshVertexDto &v = mesh->vertices[primitive.firstVertex + i];
            const double c[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], c[k]);
                mx[k] = std::max(mx[k], c[k]);
            }
        }
        double span[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        std::sort(span, span + 3);
        if (!(span[2] > 0.0)) {
            ++emptyPrimitives;
            continue;
        }
        longestSpan = std::max(longestSpan, span[2]);
        if (span[0] <= 0.02 * span[2])
            ++sheetPrimitives;
        else
            ++solidPrimitives;
    }

    // 画面：导入后会话已经把视口聚焦到图层。9.8 万个基元的上屏是分批的，
    // 所以等到连续两帧几乎不再变化才判读，最多等 60 秒。
    QImage shot;
    bool hadPrevious = false;
    int settleChanged = 0;
    QElapsedTimer settleClock;
    settleClock.start();
    while (settleClock.elapsed() < 60000) {
        waitFor(1000);
        const QImage next =
            view ? view->captureImageOffscreen(shotSize, View3D::CaptureOptions()) : QImage();
        if (next.isNull()) {
            shot = next;
            break;
        }
        int changed = next.width() * next.height();
        if (hadPrevious && next.size() == shot.size()) {
            const QImage a = shot.convertToFormat(QImage::Format_ARGB32);
            const QImage b = next.convertToFormat(QImage::Format_ARGB32);
            changed = 0;
            for (int y = 0; y < b.height(); ++y) {
                const QRgb *pa = reinterpret_cast<const QRgb *>(a.scanLine(y));
                const QRgb *pb = reinterpret_cast<const QRgb *>(b.scanLine(y));
                for (int x = 0; x < b.width(); ++x)
                    if (pa[x] != pb[x])
                        ++changed;
            }
        }
        shot = next;
        settleChanged = changed;
        if (hadPrevious && changed * 100 < next.width() * next.height())
            break;
        hadPrevious = true;
    }
    const ShotStats stats = analyseShot(shot, background);
    const QString dumpPrefix = qEnvironmentVariable("VIEW3D_3DS_DUMP");
    if (!dumpPrefix.isEmpty() && !shot.isNull())
        shot.save(dumpPrefix + QStringLiteral(".png"));
    View3D::ViewportCameraState camera;
    if (view)
        view->cameraState(&camera);
    if (shot.isNull())
        return fail(QStringLiteral("离屏抓图失败，无法判读画面"));
    // 画面必须真的动起来：只有 1-2 种颜色说明整片是同一个灰，等于没渲染。
    if (stats.distinctColours <= 2)
        return fail(QStringLiteral("3DS 没有真正渲染出内容（画面只有 %1 种颜色，lum=%2）")
                        .arg(stats.distinctColours)
                        .arg(stats.meanLum));
    std::cerr << "VIEW3D_3DS_SHAPE sheet=" << sheetPrimitives << " solid=" << solidPrimitives
              << " empty=" << emptyPrimitives << " longest_m=" << longestSpan
              << " colours=" << stats.distinctColours << " lum=" << stats.meanLum
              << " dark_permille=" << stats.darkPermille
              << " settle_ms=" << settleClock.elapsed() << " settle_changed=" << settleChanged
              << " camera_target=(" << camera.target[0] << ", " << camera.target[1] << ", "
              << camera.target[2] << ")" << std::endl;
    if (!dumpPrefix.isEmpty())
        std::cerr << "VIEW3D_3DS_DUMPED path=" << dumpPrefix.toStdString() << ".png" << std::endl;

    std::cerr << "VIEW3D_3DS_RELEASE begin vertices=" << mesh->vertices.size() << std::endl;
    layers->removeLayer(group);
    std::cerr << "VIEW3D_3DS_RELEASE removed" << std::endl;
    QElapsedTimer releaseTimer;
    releaseTimer.start();
    while (releaseTimer.elapsed() < 60000 && scene->registeredVertexCount() > baseVertices)
        waitFor(50);
    std::cerr << "VIEW3D_3DS_RELEASE settled ms=" << releaseTimer.elapsed() << " still_owned="
              << (scene->registeredVertexCount() - baseVertices) << std::endl;

    std::cerr << "VIEW3D_3DS_DATASET_OK ms=" << elapsedMs << " vertices=" << vertexCount
              << " primitives=" << primitiveCount << " nodes=" << nodeCount
              << " cpu_mb=" << cpuBytes / 1048576 << std::endl;
    return true;
}

// VIEW3D_MODEL_FILE=<path>：按「打开模型」真实入口导入任意模型，随后走与右键
// 拖拽同一条 orbitCamera 路径做小步旋转探针，量每步耗时、被拒步数与俯仰角，
// 用来复现"打开后旋转不顺畅、操作一定程度就转不动"。未设该环境变量时跳过。
bool verifyModelFileSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString path = qEnvironmentVariable("VIEW3D_MODEL_FILE");
    if (path.isEmpty()) {
        std::cerr << "VIEW3D_MODEL_FILE_SKIPPED reason=env_unset" << std::endl;
        return true;
    }
    if (!QFileInfo::exists(path))
        return fail(QStringLiteral("模型文件不存在：%1").arg(path));

    auto *container = application.getPluginContainer();
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *openRegistry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!scene || !layers || !openRegistry || !layout)
        return fail(QStringLiteral("模型文件回归缺少场景/图层/打开数据服务"));

    View3D::IDataOpener *modelOpener = nullptr;
    for (View3D::IDataOpener *opener : openRegistry->dataOpeners()) {
        if (opener && opener->displayName().contains(QStringLiteral("模型文件"))) {
            modelOpener = opener;
            break;
        }
    }
    if (!modelOpener)
        return fail(QStringLiteral("没有模型打开器"));

    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    // 导入选项对话框是模态的：无头跑必须在下一轮事件循环里替用户点确定。
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isModal() && dialog->isVisible() &&
                dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                dialog->accept();
                return;
            }
        }
    });

    QSet<QString> beforeKeys;
    for (const QString &key : scene->modelLayerKeys())
        beforeKeys.insert(key);
    const std::uint64_t baseVertices = scene->registeredVertexCount();
    const View3D::LayerId group = layers->addGroup(QStringLiteral("模型文件回归组"));
    QString openError;
    if (!modelOpener->openData(path, group, &openError))
        return fail(QStringLiteral("模型打开器拒绝该文件：%1").arg(openError));

    QElapsedTimer clock;
    clock.start();
    const int waitMs = qEnvironmentVariableIntValue("VIEW3D_MODEL_WAIT_MS") > 0
        ? qEnvironmentVariableIntValue("VIEW3D_MODEL_WAIT_MS")
        : 900000;
    QString layerKey;
    while (clock.elapsed() < waitMs && layerKey.isEmpty()) {
        waitFor(200);
        for (const QString &key : scene->modelLayerKeys()) {
            if (!beforeKeys.contains(key)) {
                layerKey = key;
                break;
            }
        }
        if (!layerKey.isEmpty())
            break;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (box && box->isVisible() &&
                box->windowTitle().contains(QStringLiteral("打开数据失败"))) {
                const QString detail = box->text() + QLatin1Char(' ') + box->informativeText();
                box->done(QMessageBox::Ok);
                return fail(QStringLiteral("模型导入失败：%1").arg(detail.trimmed()));
            }
        }
    }
    if (layerKey.isEmpty())
        return fail(QStringLiteral("等待 %1 秒仍没有出图层").arg(waitMs / 1000));
    const qint64 importMs = clock.elapsed();

    const std::shared_ptr<const View3D::ModelMeshDto> mesh = scene->modelLayerMesh(layerKey);
    if (!mesh || mesh->vertices.empty())
        return fail(QStringLiteral("模型图层没有可用几何"));
    View3D::RenderBounds bounds;
    const bool hasBounds = scene->modelLayerBounds(layerKey, &bounds);

    waitFor(qEnvironmentVariableIntValue("VIEW3D_MODEL_SETTLE_MS") > 0
                ? qEnvironmentVariableIntValue("VIEW3D_MODEL_SETTLE_MS")
                : 3000);
    View3D::IRenderSurface *view = layout->primaryView();
    if (!view)
        return fail(QStringLiteral("没有主视口"));

    // 渲染成本代理：离屏抓帧（与屏幕同一套场景绘制）分别量开/关阴影。
    view->captureImageOffscreen(QSize(512, 512), View3D::CaptureOptions());
    QElapsedTimer renderClock;
    renderClock.start();
    const QImage shot = view->captureImageOffscreen(QSize(512, 512),
                                                    View3D::CaptureOptions());
    const qint64 captureMs = renderClock.elapsed();
    qint64 captureNoShadowMs = -1;
    const bool shadowsWere = view->shadowStyle().enabled;
    if (shadowsWere) {
        view->setShadowsEnabled(false);
        view->captureImageOffscreen(QSize(512, 512), View3D::CaptureOptions());
        QElapsedTimer noShadowClock;
        noShadowClock.start();
        view->captureImageOffscreen(QSize(512, 512), View3D::CaptureOptions());
        captureNoShadowMs = noShadowClock.elapsed();
        view->setShadowsEnabled(true);
    }
    // 交互帧率：直接数渲染线程的帧计数，要比抓图耗时口径干净。
    auto *renderService = container
        ? container->objectAs<View3D::IRenderService>(View3D::ServiceIds::Renderer)
        : nullptr;
    const auto measureFps = [&](int milliseconds) -> double {
        if (!renderService)
            return -1.0;
        const std::uint64_t frames0 = renderService->frameCount();
        QElapsedTimer timer;
        timer.start();
        waitFor(milliseconds);
        const double seconds = timer.elapsed() / 1000.0;
        return seconds > 0.0
            ? static_cast<double>(renderService->frameCount() - frames0) / seconds
            : -1.0;
    };
    const double fpsShadows = measureFps(1500);
    view->setShadowsEnabled(false);
    const double fpsNoShadows = measureFps(1500);
    view->setShadowsEnabled(shadowsWere);
    const QString dumpPrefix = qEnvironmentVariable("VIEW3D_MODEL_DUMP");
    if (!dumpPrefix.isEmpty() && !shot.isNull())
        shot.save(dumpPrefix + QStringLiteral("_model.png"));

    const auto distance3 = [](const std::array<double, 3> &a,
                              const std::array<double, 3> &b) {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        const double dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // 旋转探针：每步 4°/3°，与右键拖拽同一入口。
    const int steps = qEnvironmentVariableIntValue("VIEW3D_MODEL_ORBIT_STEPS") > 0
        ? qEnvironmentVariableIntValue("VIEW3D_MODEL_ORBIT_STEPS")
        : 40;
    int blocked = 0;
    double maxStepMs = 0.0;
    double sumStepMs = 0.0;
    double pitchDeg = 0.0;
    double radius = 0.0;
    for (int step = 0; step < steps; ++step) {
        View3D::ViewportCameraState before;
        if (!view->cameraState(&before))
            return fail(QStringLiteral("读不到相机状态"));
        QElapsedTimer stepClock;
        stepClock.start();
        view->orbitCamera(4.0, 3.0);
        const double stepMs = stepClock.nsecsElapsed() / 1.0e6;
        View3D::ViewportCameraState after;
        if (!view->cameraState(&after))
            return fail(QStringLiteral("旋转后读不到相机状态"));
        maxStepMs = std::max(maxStepMs, stepMs);
        sumStepMs += stepMs;
        pitchDeg = std::asin(qBound(-1.0, after.direction[1], 1.0)) * 180.0 /
            std::acos(-1.0);
        radius = distance3(after.position, after.target);
        if (distance3(before.position, after.position) < 1.0e-6 &&
            distance3(before.direction, after.direction) < 1.0e-7) {
            ++blocked;
        }
        waitFor(16);
    }
    std::cerr << "VIEW3D_MODEL_ORBIT steps=" << steps << " blocked=" << blocked
              << " pitch_deg=" << pitchDeg << " radius_m=" << radius
              << " max_step_ms=" << maxStepMs
              << " avg_step_ms=" << (sumStepMs / std::max(1, steps)) << std::endl;

    // 顶到俯仰限位之后，水平旋转必须仍然有效：旧实现会把整次旋转都拒掉，
    // 表现就是"转到一定程度就再也转不动"。
    for (int push = 0; push < 4; ++push) {
        view->orbitCamera(0.0, 6.0);
        waitFor(16);
    }
    View3D::ViewportCameraState limitBefore;
    const bool limitReadBefore = view->cameraState(&limitBefore);
    view->orbitCamera(30.0, 0.0);
    View3D::ViewportCameraState limitAfter;
    const bool limitReadAfter = view->cameraState(&limitAfter);
    const double yawAtLimitMoved =
        limitReadBefore && limitReadAfter ? distance3(limitBefore.position, limitAfter.position)
                                          : -1.0;
    std::cerr << "VIEW3D_MODEL_LIMIT pitch_deg="
              << (limitReadAfter
                      ? std::asin(qBound(-1.0, limitAfter.direction[1], 1.0)) * 180.0 /
                            std::acos(-1.0)
                      : 0.0)
              << " yaw30_moved_m=" << yawAtLimitMoved << std::endl;

    std::cerr << "VIEW3D_MODEL_FILE_OK ms=" << importMs
              << " vertices=" << mesh->vertices.size()
              << " primitives=" << mesh->primitives.size()
              << " nodes=" << mesh->nodes.size()
              << " cpu_mb=" << View3D::modelMeshCpuBytes(*mesh) / 1048576
              << " bounds_valid=" << (hasBounds && bounds.valid ? 1 : 0);
    if (hasBounds && bounds.valid) {
        std::cerr << " bounds_min=" << bounds.minimum[0] << "," << bounds.minimum[1] << ","
                  << bounds.minimum[2] << " bounds_max=" << bounds.maximum[0] << ","
                  << bounds.maximum[1] << "," << bounds.maximum[2];
    }
    std::cerr << " capture_ms=" << captureMs << " capture_no_shadow_ms=" << captureNoShadowMs
              << " fps_shadows=" << fpsShadows << " fps_no_shadows=" << fpsNoShadows
              << " still_owned=" << (scene->registeredVertexCount() - baseVertices)
              << std::endl;
    return true;
}

bool verifyOsgbDatasetSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString directory = qEnvironmentVariable("VIEW3D_OSBG_DIR");
    if (directory.isEmpty()) {
        std::cerr << "VIEW3D_OSBG_SKIPPED reason=env_unset" << std::endl;
        return true;
    }
    auto *container = application.getPluginContainer();
    auto *loader = container
        ? container->objectAs<View3D::IOsbLoader>(View3D::ServiceIds::OsbLoader)
        : nullptr;
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    if (!loader || !scene)
        return fail(QStringLiteral("倾斜摄影服务或场景不可用"));

    const std::uint64_t baseVertices = scene->registeredVertexCount();
    // 背景参考必须在加载数据集之前采：天空是屏幕空间的竖直渐变，与相机姿态无关，
    // 所以这一张 512 图可以服务后面所有同尺寸视角的空白判定。
    auto* layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    View3D::IRenderSurface* view = layout ? layout->primaryView() : nullptr;
    const QSize shotSize(512, 512);
    QImage backgroundShot =
        view ? view->captureImageOffscreen(shotSize, View3D::CaptureOptions()) : QImage();
    QElapsedTimer loadClock;
    loadClock.start();
    if (!loader->loadDataset(directory))
        return fail(loader->lastError());

    const QString groupKey =
        QStringLiteral("osgb:") + QFileInfo(directory).fileName();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 && scene->registeredVertexCount() <= baseVertices) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint64_t added = scene->registeredVertexCount() - baseVertices;
    const qint64 firstContentMs = loadClock.elapsed();
    if (added == 0)
        return fail(QStringLiteral("倾斜摄影瓦片未产生顶点数据"));
    if (scene->tileCount(groupKey) == 0)
        return fail(QStringLiteral("倾斜摄影瓦片组不可见：%1").arg(groupKey));
    View3D::RenderBounds bounds;
    if (!scene->tileGroupBounds(groupKey, &bounds) || !bounds.valid)
        return fail(QStringLiteral("倾斜摄影瓦片组缺少包围盒"));
    const std::uint32_t rootTiles = scene->tileCount(groupKey);
    if (rootTiles == 0)
        return fail(QStringLiteral("倾斜摄影瓦片组没有根瓦片"));

    // ── 参照系：metadata 的 SRS + "减 SRSOrigin 后 (E,U,-N)" 的刚体矩阵 ────────
    QString osgbCrs;
    if (auto *layers = container->objectAs<View3D::ISceneLayerService>(
            View3D::ServiceIds::SceneLayers)) {
        const View3D::LayerId tileLayer =
            layers->layerByRenderKey(QStringLiteral("tile:") + groupKey);
        if (tileLayer.isNull())
            return fail(QStringLiteral("倾斜摄影瓦片组没有对应图层"));
        osgbCrs = layers->layerSourceCrs(tileLayer);
        // metadata 的 SRS 可能是 EPSG 码，也可能是整段 WKT（大雁塔就是后者）。
        if (osgbCrs.trimmed().isEmpty())
            return fail(QStringLiteral("倾斜摄影图层没有记录 metadata 的 SRS"));
        const View3D::ILayer::ReferenceFrame frame =
            layers->layerReferenceFrame(tileLayer);
        const QString problem = rigidFrameProblem(frame);
        if (!problem.isEmpty())
            return fail(QStringLiteral("倾斜摄影参照系矩阵异常：%1").arg(problem));
        // 轴交换必须是 (E,N,U)→(E,U,-N)：第二行 (0,0,1)、第三行 (0,-1,0)。
        const std::array<double, 16> &m = frame.matrix;
        if (qAbs(m[4]) > 1e-9 || qAbs(m[5]) > 1e-9 || qAbs(m[6] - 1.0) > 1e-9 ||
            qAbs(m[8]) > 1e-9 || qAbs(m[9] + 1.0) > 1e-9 || qAbs(m[10]) > 1e-9)
            return fail(QStringLiteral("倾斜摄影轴交换不是 (E,U,-N)：%1,%2,%3 / %4,%5,%6")
                            .arg(m[4]).arg(m[5]).arg(m[6]).arg(m[8]).arg(m[9]).arg(m[10]));
        // metadata 有 SRSOrigin：面板要给出 SRSOrigin，并由它反算出经纬度。
        if (!frame.origin.isEmpty() &&
            !frame.origin.startsWith(QStringLiteral("SRSOrigin")))
            return fail(QStringLiteral("倾斜摄影原点行应为 SRSOrigin：%1").arg(frame.origin));
        if (!frame.origin.isEmpty() && frame.position.isEmpty())
            return fail(QStringLiteral("倾斜摄影没有由 SRSOrigin 反算出经纬度"));
        std::cerr << "VIEW3D_OSBG_REF_FRAME_OK crs=" << osgbCrs.toStdString()
                  << " position=" << frame.position.toStdString()
                  << " origin=" << frame.origin.toStdString() << std::endl;
    }

    // LOD 必须按数据内嵌阈值继续向下钻取：驻留瓦片数要增长。
    timer.restart();
    while (timer.elapsed() < 4000 && scene->tileCount(groupKey) <= rootTiles) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint32_t refinedTiles = scene->tileCount(groupKey);
    const qint64 refineMs = loadClock.elapsed();
    if (refinedTiles <= rootTiles)
        return fail(QStringLiteral("倾斜摄影未向下切换 LOD（驻留瓦片仍为 %1）")
                        .arg(refinedTiles));
    // 再跑一段时间看吞吐：统计稳态驻留瓦片与顶点规模（只打印，不断言，避免机器差异）。
    timer.restart();
    while (timer.elapsed() < 2500) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint32_t steadyTiles = scene->tileCount(groupKey);
    const std::uint64_t steadyVertices = scene->registeredVertexCount() - baseVertices;
    // 姿态检查：EPSG 投影里 z 是高程且地面在 SRSOrigin 上，换到渲染器的 +Y 上之后，
    // 模型应当站在网格平面上方（y 下界贴近 0），而不是横躺或斜插进地里。
    View3D::RenderBounds steadyBounds;
    if (!scene->tileGroupBounds(groupKey, &steadyBounds) || !steadyBounds.valid)
        return fail(QStringLiteral("倾斜摄影稳态包围盒不可用"));
    const double height = steadyBounds.maximum[1] - steadyBounds.minimum[1];
    const double eastWest = steadyBounds.maximum[0] - steadyBounds.minimum[0];
    const double northSouth = steadyBounds.maximum[2] - steadyBounds.minimum[2];
    std::cerr << "VIEW3D_OSBG_STREAM steady_tiles=" << steadyTiles
              << " steady_vertices=" << steadyVertices
              << " textures=" << scene->registeredTextureCount()
              << " min=" << steadyBounds.minimum[0] << "," << steadyBounds.minimum[1] << ","
              << steadyBounds.minimum[2] << " max=" << steadyBounds.maximum[0] << ","
              << steadyBounds.maximum[1] << "," << steadyBounds.maximum[2]
              << " size=" << eastWest << "," << height << "," << northSouth << std::endl;
    if (height <= 0.0)
        return fail(QStringLiteral("倾斜摄影竖直范围为 0，姿态映射有误"));
    // 姿态判据只看"竖着还是躺着"：摄影测量成果的竖直跨度必须明显小于水平跨度，
    // 否则 (E,N,U)→(E,U,−N) 这根轴接错了。
    // 不能用"y 下界贴近 0"当判据：那等于假定 SRSOrigin 的高程正好在实地面上，
    // 而大雁塔（西安，地面高程约 404 m）的 metadata 只把东/北归零、z 写 0，
    // 数据合法地悬在 400 m 高程上，用它判"没站在地面上"会把整个用例在前置断言处
    // 截断，后面所有 LOD 判据都跑不到。
    const double horizontal = std::max(eastWest, northSouth);
    // 阈值从 0.5 放宽到 1.0：重庆那份只有 118×60 m 的小片，竖直/水平 = 0.51 是合法值，
    // 0.5 会把整个用例在前置断言处截断（后面的 LOD 与离屏判据全部跑不到）。
    // 轴接错时竖直跨度会等于某根水平轴，比值必然 ≥ 1，所以这根线仍然抓得住真错。
    if (height >= horizontal)
        return fail(QStringLiteral("倾斜摄影姿态不对：竖直跨度 %1 不小于水平跨度 %2")
                        .arg(height)
                        .arg(horizontal));
    if (std::abs(steadyBounds.minimum[1]) > 0.5 * height)
        std::cerr << "VIEW3D_OSBG_ELEVATION y_min=" << steadyBounds.minimum[1]
                  << " height=" << height
                  << "（数据集高程不为 0，只记录不判失败）" << std::endl;

    if (!layout)
        return fail(QStringLiteral("视口布局服务不可用"));

    // 推进到街景距离：视野内的深层瓦片必须继续被请求，驻留数量要再次增长。
    const std::uint32_t farTiles = steadyTiles;
    if (!view)
        return fail(QStringLiteral("缺少主视口，无法验证靠近后的 LOD 切换"));
    View3D::ViewportCameraState camera;
    if (!view->cameraState(&camera))
        return fail(QStringLiteral("主视口相机状态不可读"));
    const double centre[3] = {0.5 * (steadyBounds.minimum[0] + steadyBounds.maximum[0]),
                              0.5 * (steadyBounds.minimum[1] + steadyBounds.maximum[1]),
                              0.5 * (steadyBounds.minimum[2] + steadyBounds.maximum[2])};
    // 离屏图尺寸 512 才够让 16×16 网格分辨出"沿瓦片轮廓"的空白形状（见函数开头）。
    View3D::ViewportCameraState cityCamera;
    view->cameraState(&cityCamera);
    const QImage cityShot =
        view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
    // 推进距离可用 VIEW3D_OSBG_NEAR_M 调整，便于在不同视角下量 LOD 收敛情况。
    const double nearDistance =
        qEnvironmentVariable("VIEW3D_OSBG_NEAR_M").toDouble() > 0.0
            ? qEnvironmentVariable("VIEW3D_OSBG_NEAR_M").toDouble()
            : 180.0;
    const double drop = 0.216 * nearDistance;
    const double back = 0.977 * nearDistance;
    camera.position = {centre[0], centre[1] + drop, centre[2] + back};
    camera.target = {centre[0], centre[1], centre[2]};
    camera.direction = {0.0, -0.216, -0.977};
    camera.up = {0.0, 1.0, 0.0};
    if (!view->setCameraState(camera))
        return fail(QStringLiteral("无法把相机推进到街景距离"));
    timer.restart();
    while (timer.elapsed() < 8000 && scene->tileCount(groupKey) <= farTiles) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint32_t nearTiles = scene->tileCount(groupKey);
    const qint64 nearMs = loadClock.elapsed();
    if (nearTiles <= farTiles) {
        // 单层或不完整成果（重庆那份只有 L18/L19，46 块在城市视角就全驻留了）永远
        // 满足不了这条。用开关降级成告警，好让后面的离屏与颜色判据能跑到。
        if (qEnvironmentVariableIsSet("VIEW3D_OSBG_ALLOW_SHALLOW")) {
            std::cerr << "VIEW3D_OSBG_SHALLOW_WARN near_tiles=" << nearTiles
                      << " far_tiles=" << farTiles << "（按开关放行，不再要求继续下钻）"
                      << std::endl;
        } else {
            return fail(QStringLiteral("靠近后未继续向下切换 LOD（驻留瓦片仍为 %1）")
                            .arg(nearTiles));
        }
    }
    // 再给流式一点时间沉降，看最细一层是否真的补满（只测量，不断言，避免机器差异）。
    timer.restart();
    while (timer.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    const std::uint32_t settledNearTiles = scene->tileCount(groupKey);
    const qint64 settledNearMs = loadClock.elapsed();

    // 观测用：停在当前机位不动若干秒，把 sse / shown / reload 的时间序列拉出来，
    // 用来判定"放大后反复刷新却不变清晰"是不是调度自激。
    const double holdSeconds = qEnvironmentVariable("VIEW3D_OSBG_HOLD_S").toDouble();
    if (holdSeconds > 0.0) {
        QElapsedTimer hold;
        hold.start();
        while (hold.elapsed() < qint64(holdSeconds * 1000.0)) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        std::cerr << "VIEW3D_OSBG_HOLD seconds=" << holdSeconds
                  << " tiles_now=" << scene->tileCount(groupKey)
                  << " vertices_now=" << (scene->registeredVertexCount() - baseVertices)
                  << std::endl;
    }

    // 相机导航探针 VIEW3D_NAV_PROBE=1：滚轮与右键拖动都必须以光标下的模型
    // 表面点为枢轴，而不是加载时框定出来的数据集中心；同时量每次拾取耗时。
    if (qEnvironmentVariableIsSet("VIEW3D_NAV_PROBE")) {
        const double probeDistance = 120.0;
        const double probeStreetY = steadyBounds.minimum[1] +
            0.2 * (steadyBounds.maximum[1] - steadyBounds.minimum[1]);
        View3D::ViewportCameraState probeCamera = camera;
        probeCamera.position = {centre[0], probeStreetY + 0.35 * probeDistance,
                                centre[2] + probeDistance};
        probeCamera.target = {centre[0], probeStreetY, centre[2]};
        const double probeDy = probeStreetY - probeCamera.position[1];
        const double probeNorm =
            std::sqrt(probeDistance * probeDistance + probeDy * probeDy);
        probeCamera.direction = {0.0, probeDy / probeNorm, -probeDistance / probeNorm};
        probeCamera.up = {0.0, 1.0, 0.0};
        if (!view->setCameraState(probeCamera))
            return fail(QStringLiteral("导航探针无法设置相机"));
        timer.restart();
        while (timer.elapsed() < 2500) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        QWidget* probeWidget = view->widget();
        if (!probeWidget || probeWidget->width() <= 0 || probeWidget->height() <= 0) {
            return fail(QStringLiteral("导航探针视口尺寸无效：%1×%2")
                            .arg(probeWidget ? probeWidget->width() : 0)
                            .arg(probeWidget ? probeWidget->height() : 0));
        }
        const double probeX = probeWidget->width() * 0.5;
        const double probeY = probeWidget->height() * 0.5;
        const auto distanceToTarget = [](const View3D::ViewportCameraState& state) {
            const double dx = state.position[0] - state.target[0];
            const double dy = state.position[1] - state.target[1];
            const double dz = state.position[2] - state.target[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const auto distance3 = [](const std::array<double, 3>& a,
                                  const std::array<double, 3>& b) {
            const double dx = a[0] - b[0];
            const double dy = a[1] - b[1];
            const double dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        double previousDistance = std::numeric_limits<double>::max();
        double totalZoomMs = 0.0;
        double maxZoomMs = 0.0;
        double finalDistance = 0.0;
        std::array<double, 3> zoomTarget {};
        bool monotonic = true;
        for (int step = 1; step <= 30; ++step) {
            QElapsedTimer stepClock;
            stepClock.start();
            const bool zoomed = view->zoomCameraAt(probeX, probeY, 1.0);
            const double stepMs = stepClock.nsecsElapsed() / 1.0e6;
            View3D::ViewportCameraState state;
            if (!zoomed || !view->cameraState(&state))
                return fail(QStringLiteral("导航探针滚轮缩放失败：第 %1 格").arg(step));
            const double distance = distanceToTarget(state);
            if (distance > previousDistance + 1.0e-6)
                monotonic = false;
            previousDistance = distance;
            totalZoomMs += stepMs;
            maxZoomMs = std::max(maxZoomMs, stepMs);
            finalDistance = distance;
            zoomTarget = state.target;
            if (step % 5 == 0) {
                std::cerr << "VIEW3D_NAV_ZOOM step=" << step
                          << " dist_m=" << distance
                          << " target_y=" << state.target[1]
                          << " target_to_centre_m="
                          << distance3(state.target, {centre[0], centre[1], centre[2]})
                          << " ms=" << stepMs << std::endl;
            }
        }
        const double targetToCentre =
            distance3(zoomTarget, {centre[0], centre[1], centre[2]});
        if (!monotonic)
            return fail(QStringLiteral("导航探针滚轮缩放未单调推进"));
        if (finalDistance > 2.0)
            return fail(QStringLiteral("导航探针滚轮缩放未能推进到近处：%1 m")
                            .arg(finalDistance));
        if (targetToCentre <= 10.0)
            return fail(QStringLiteral("导航探针缩放枢轴仍是数据集中心：%1 m")
                            .arg(targetToCentre));
        const double zoomAvgMs = totalZoomMs / 30.0;
        if (maxZoomMs > 100.0)
            return fail(QStringLiteral("导航探针单次缩放拾取过慢：%1 ms").arg(maxZoomMs));
        const QImage closeShot =
            view->captureImageOffscreen(QSize(512, 512), View3D::CaptureOptions());
        const ShotStats closeStats = analyseShot(closeShot, backgroundShot);
        const QString navDumpPrefix = qEnvironmentVariable("VIEW3D_OSBG_DUMP");
        if (!navDumpPrefix.isEmpty())
            closeShot.save(navDumpPrefix + QStringLiteral("_nav_close.png"));

        // 右键按下时枢轴应切到鼠标下的表面点，随后的轨道旋转必须绕该点。
        if (!view->setCameraState(probeCamera))
            return fail(QStringLiteral("导航探针无法复位相机"));
        timer.restart();
        while (timer.elapsed() < 600) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QPoint probeGlobal = probeWidget->mapToGlobal(
            QPoint(qRound(probeX), qRound(probeY)));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(probeX, probeY),
                          QPointF(probeGlobal), Qt::RightButton, Qt::RightButton,
                          Qt::NoModifier);
        QApplication::sendEvent(probeWidget, &press);
        View3D::ViewportCameraState pivoted;
        if (!view->cameraState(&pivoted))
            return fail(QStringLiteral("导航探针无法读取轨道枢轴"));
        const double pivotToCentre =
            distance3(pivoted.target, {centre[0], centre[1], centre[2]});
        if (pivotToCentre <= 10.0)
            return fail(QStringLiteral("右键按下后枢轴仍是数据集中心：%1 m")
                            .arg(pivotToCentre));
        const double pivotToZoom = distance3(pivoted.target, zoomTarget);
        if (pivotToZoom > 1.0)
            return fail(QStringLiteral("右键枢轴与滚轮枢轴不一致：%1 m").arg(pivotToZoom));
        const double pivotRadius = distanceToTarget(pivoted);
        if (!view->orbitCamera(10.0, 0.0))
            return fail(QStringLiteral("导航探针轨道旋转失败"));
        View3D::ViewportCameraState orbited;
        if (!view->cameraState(&orbited))
            return fail(QStringLiteral("导航探针旋转后相机不可读"));
        const double orbitTargetDrift = distance3(orbited.target, pivoted.target);
        const double orbitRadiusDrift = qAbs(distanceToTarget(orbited) - pivotRadius);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(probeX, probeY),
                            QPointF(probeGlobal), Qt::RightButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(probeWidget, &release);
        std::cerr << "VIEW3D_NAV_PIVOT pivot_to_centre_m=" << pivotToCentre
                  << " pivot_to_zoom_m=" << pivotToZoom
                  << " pivot_radius_m=" << pivotRadius
                  << " target_drift=" << orbitTargetDrift
                  << " radius_drift=" << orbitRadiusDrift << std::endl;
        // Ogre 相机运算走 Real(float)，120 m 半径下 1 mm 内视为稳定。
        if (orbitTargetDrift > 1.0e-3 || orbitRadiusDrift > 1.0e-3) {
            return fail(QStringLiteral("轨道旋转没有绕鼠标拾取点：target_drift=%1 "
                                       "radius_drift=%2")
                            .arg(orbitTargetDrift)
                            .arg(orbitRadiusDrift));
        }
        // 起始旋转不得跳变：偏屏点击/天空点击后只走一格（0.35°）轨道，
        // 视线方向变化应约等于一格，而不是瞬间对准枢轴。
        const auto onePixelTurn = [&](double clickX, double clickY, const char* label) {
            if (!view->setCameraState(probeCamera))
                return -1.0;
            timer.restart();
            while (timer.elapsed() < 400) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            const QPoint global = probeWidget->mapToGlobal(
                QPoint(qRound(clickX), qRound(clickY)));
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(clickX, clickY),
                              QPointF(global), Qt::RightButton, Qt::RightButton,
                              Qt::NoModifier);
            QApplication::sendEvent(probeWidget, &press);
            View3D::ViewportCameraState before;
            const bool started = view->cameraState(&before);
            const bool orbited = started && view->orbitCamera(0.35, 0.0);
            View3D::ViewportCameraState after;
            const bool readBack = orbited && view->cameraState(&after);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(clickX, clickY),
                                QPointF(global), Qt::RightButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(probeWidget, &release);
            if (!readBack)
                return -1.0;
            const double dot = std::clamp(
                before.direction[0] * after.direction[0] +
                    before.direction[1] * after.direction[1] +
                    before.direction[2] * after.direction[2],
                -1.0, 1.0);
            const double turnDegrees = std::acos(dot) * 180.0 / 3.141592653589793;
            const double dx = after.position[0] - before.position[0];
            const double dy = after.position[1] - before.position[1];
            const double dz = after.position[2] - before.position[2];
            std::cerr << "VIEW3D_NAV_ORBIT test=" << label
                      << " turn_deg=" << turnDegrees
                      << " move_m=" << std::sqrt(dx * dx + dy * dy + dz * dz)
                      << " click=" << clickX << "," << clickY << std::endl;
            return turnDegrees;
        };
        const double offCenterTurn = onePixelTurn(probeWidget->width() * 0.35,
                                                  probeWidget->height() * 0.55,
                                                  "off_center");
        const double skyTurn = onePixelTurn(probeWidget->width() * 0.92,
                                            probeWidget->height() * 0.06, "sky");
        if (offCenterTurn < 0.0 || skyTurn < 0.0)
            return fail(QStringLiteral("导航探针起始旋转测量失败"));
        if (offCenterTurn > 1.5 || skyTurn > 1.5) {
            return fail(QStringLiteral("起始旋转发生跳变：off_center=%1° sky=%2°")
                            .arg(offCenterTurn)
                            .arg(skyTurn));
        }
        // 连续轨道旋转不得积累侧倾：相机右向量的竖直分量（right.y）就是滚转量，
        // 非零即地平线左高右低。偏屏枢轴下连续做水平+垂直旋转 30 格。
        if (!view->setCameraState(probeCamera))
            return fail(QStringLiteral("侧倾探针无法复位相机"));
        timer.restart();
        while (timer.elapsed() < 400) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const double rollClickX = probeWidget->width() * 0.35;
        const double rollClickY = probeWidget->height() * 0.55;
        const QPoint rollGlobal = probeWidget->mapToGlobal(
            QPoint(qRound(rollClickX), qRound(rollClickY)));
        QMouseEvent rollPress(QEvent::MouseButtonPress,
                              QPointF(rollClickX, rollClickY), QPointF(rollGlobal),
                              Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(probeWidget, &rollPress);
        const auto rollDegrees = [](const View3D::ViewportCameraState& state) {
            const std::array<double, 3> right {
                state.direction[1] * state.up[2] - state.direction[2] * state.up[1],
                state.direction[2] * state.up[0] - state.direction[0] * state.up[2],
                state.direction[0] * state.up[1] - state.direction[1] * state.up[0]};
            const double length = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                            right[2] * right[2]);
            if (length < 1.0e-9)
                return 0.0;
            return std::asin(std::clamp(right[1] / length, -1.0, 1.0)) * 180.0 /
                3.141592653589793;
        };
        double maxRollDegrees = 0.0;
        double finalRollDegrees = 0.0;
        for (int step = 0; step < 30; ++step) {
            if (!view->orbitCamera(1.0, 0.4))
                return fail(QStringLiteral("侧倾探针轨道旋转失败：第 %1 格").arg(step));
            View3D::ViewportCameraState state;
            if (!view->cameraState(&state))
                return fail(QStringLiteral("侧倾探针相机不可读"));
            finalRollDegrees = rollDegrees(state);
            maxRollDegrees = std::max(maxRollDegrees, qAbs(finalRollDegrees));
        }
        QMouseEvent rollRelease(QEvent::MouseButtonRelease,
                                QPointF(rollClickX, rollClickY), QPointF(rollGlobal),
                                Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(probeWidget, &rollRelease);
        std::cerr << "VIEW3D_NAV_ROLL steps=30 roll_max_deg=" << maxRollDegrees
                  << " roll_final_deg=" << finalRollDegrees
                  << " click=" << rollClickX << "," << rollClickY << std::endl;
        if (maxRollDegrees > 1.0) {
            return fail(QStringLiteral("连续旋转积累侧倾 %1°（地平线左高右低）")
                            .arg(finalRollDegrees));
        }
        if (!view->setCameraState(probeCamera))
            return fail(QStringLiteral("侧倾探针无法恢复相机"));
        std::cerr << "VIEW3D_NAV_PROBE_OK steps=30"
                  << " zoom_avg_ms=" << zoomAvgMs
                  << " zoom_max_ms=" << maxZoomMs
                  << " final_dist_m=" << finalDistance
                  << " target_to_centre_m=" << targetToCentre
                  << " pivot_to_centre_m=" << pivotToCentre
                  << " pivot_radius_m=" << pivotRadius
                  << " lum=" << closeStats.meanLum
                  << " widget=" << probeWidget->width() << "x" << probeWidget->height()
                  << std::endl;
        if (!view->setCameraState(probeCamera))
            return fail(QStringLiteral("导航探针无法恢复相机"));
    }

    // 连续推近拍摄：VIEW3D_OSBG_ZOOM_S 秒内从 800 m 匀速对数推进到 60 m，
    // 每 ~120 ms 抓一张离屏图，统计相邻帧的平均像素差。切换生硬/闪空会表现为
    // 远高于相邻帧的 MAD 尖峰，最差帧存盘供直接判读。
    const double zoomSeconds = qEnvironmentVariable("VIEW3D_OSBG_ZOOM_S").toDouble();
    if (zoomSeconds > 0.0 && view) {
        const double zoomStreetY =
            steadyBounds.minimum[1] + 0.2 * (steadyBounds.maximum[1] - steadyBounds.minimum[1]);
        const QString zoomDumpPrefix = qEnvironmentVariable("VIEW3D_OSBG_DUMP");
        const auto meanAbsDiff = [](const QImage& a, const QImage& b) {
            if (a.size() != b.size() || a.isNull())
                return 0.0;
            const QImage left = a.convertToFormat(QImage::Format_ARGB32);
            const QImage right = b.convertToFormat(QImage::Format_ARGB32);
            std::uint64_t sum = 0;
            std::uint64_t count = 0;
            for (int y = 0; y < left.height(); ++y) {
                const auto* la = reinterpret_cast<const QRgb*>(left.constScanLine(y));
                const auto* lb = reinterpret_cast<const QRgb*>(right.constScanLine(y));
                for (int x = 0; x < left.width(); ++x) {
                    const QRgb pa = la[x];
                    const QRgb pb = lb[x];
                    sum += std::abs(qRed(pa) - qRed(pb));
                    sum += std::abs(qGreen(pa) - qGreen(pb));
                    sum += std::abs(qBlue(pa) - qBlue(pb));
                    count += 3;
                }
            }
            return count > 0 ? double(sum) / double(count) : 0.0;
        };
        const double startDistance = 800.0;
        const double endDistance = 60.0;
        QElapsedTimer zoom;
        zoom.start();
        QImage previous;
        QImage worst;
        double worstMad = -1.0;
        double worstDistance = 0.0;
        int frame = 0;
        const qint64 zoomTotal = qint64(zoomSeconds * 1000.0);
        while (zoom.elapsed() < zoomTotal) {
            const double t = std::clamp(
                double(zoom.elapsed()) / double(std::max<qint64>(1, zoomTotal)), 0.0, 1.0);
            const double distance =
                startDistance * std::pow(endDistance / startDistance, t);
            View3D::ViewportCameraState zoomCamera = camera;
            zoomCamera.position = {centre[0], zoomStreetY + 0.35 * distance,
                                   centre[2] + distance};
            zoomCamera.target = {centre[0], zoomStreetY, centre[2]};
            const double dy = zoomStreetY - zoomCamera.position[1];
            const double norm = std::sqrt(distance * distance + dy * dy);
            zoomCamera.direction = {0.0, dy / norm, -distance / norm};
            zoomCamera.up = {0.0, 1.0, 0.0};
            view->setCameraState(zoomCamera);
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(40);
            if (frame % 3 == 0) {
                const QImage shot =
                    view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
                const double mad = meanAbsDiff(previous, shot);
                const int lum = analyseShot(shot, backgroundShot).meanLum;
                std::cerr << "VIEW3D_OSBG_ZOOM frame=" << frame
                          << " dist_m=" << distance << " mad=" << mad
                          << " lum=" << lum << std::endl;
                if (mad > worstMad) {
                    worstMad = mad;
                    worstDistance = distance;
                    worst = shot;
                }
                previous = shot;
                if (!zoomDumpPrefix.isEmpty() && frame % 15 == 0)
                    shot.save(zoomDumpPrefix + QStringLiteral("_zoom%1.png").arg(frame));
            }
            ++frame;
        }
        if (!zoomDumpPrefix.isEmpty() && !worst.isNull())
            worst.save(zoomDumpPrefix + QStringLiteral("_zoom_worst.png"));
        std::cerr << "VIEW3D_OSBG_ZOOM_DONE frames=" << frame
                  << " worst_mad=" << worstMad << " worst_dist_m=" << worstDistance
                  << std::endl;
        view->setCameraState(camera);
        timer.restart();
        while (timer.elapsed() < 1200) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    }

    // 脚本化漫游：VIEW3D_OSBG_ROAM_S 秒内绕数据集转多圈并反复拉近推远。
    // 每完成一整圈记录一次驻留顶点数——同一机位下若数值逐圈变大，就说明只加载不卸载。
    const double roamSeconds = qEnvironmentVariable("VIEW3D_OSBG_ROAM_S").toDouble();
    if (roamSeconds > 0.0) {
        const int revolutions = 3;
        const double kPi = 3.141592653589793;
        QVector<std::uint64_t> cycleEnds;
        QElapsedTimer roam;
        roam.start();
        const qint64 total = qint64(roamSeconds * 1000.0);
        while (roam.elapsed() < total) {
            const double phase = double(roam.elapsed()) / double(total);
            const double angle = phase * revolutions * 2.0 * kPi;
            const double spread = 80.0 + 320.0 * std::abs(std::sin(phase * revolutions * kPi));
            View3D::ViewportCameraState roaming = camera;
            roaming.position = {centre[0] + std::sin(angle) * spread, centre[1] + spread * 0.4,
                                centre[2] + std::cos(angle) * spread};
            roaming.target = {centre[0], centre[1], centre[2]};
            const double dx = centre[0] - roaming.position[0];
            const double dy = centre[1] - roaming.position[1];
            const double dz = centre[2] - roaming.position[2];
            const double norm = std::sqrt(dx * dx + dy * dy + dz * dz);
            roaming.direction = {dx / norm, dy / norm, dz / norm};
            view->setCameraState(roaming);
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
            const int completed = int(phase * revolutions + 1e-9);
            while (cycleEnds.size() <= completed)
                cycleEnds.append(scene->registeredVertexCount());
        }
        QByteArray ends;
        for (const std::uint64_t value : std::as_const(cycleEnds)) {
            if (!ends.isEmpty())
                ends.append(',');
            ends.append(QByteArray::number(value));
        }
        std::cerr << "VIEW3D_OSBG_ROAM seconds=" << roamSeconds
                  << " cycles=" << cycleEnds.size() << " vertices_per_cycle=" << ends.constData()
                  << std::endl;
        if (cycleEnds.size() >= 2 && cycleEnds.back() > cycleEnds.first() * 3 / 2 + 1000)
            return fail(QStringLiteral("漫游后驻留顶点逐圈增长（%1 → %2）：只加载不卸载")
                            .arg(cycleEnds.first())
                            .arg(cycleEnds.back()));
        // 回到街景机位，后面的截图与卸载断言仍按原姿态判定。
        view->setCameraState(camera);
        timer.restart();
        while (timer.elapsed() < 1500) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    }

    const QImage loadedShot =
        view ? view->captureImageOffscreen(shotSize, View3D::CaptureOptions()) : QImage();
    QImage coverRef = backgroundShot;
    const QString dumpPrefix = qEnvironmentVariable("VIEW3D_OSBG_DUMP");
    // 验收阈值：footprint 内允许的背景占比（千分比）。0 或未设 = 用默认 5。
    int blankMax = qEnvironmentVariable("VIEW3D_OSBG_BLANK_MAX").toInt();
    if (blankMax <= 0)
        blankMax = 5;
    const bool coverageStrict =
        qEnvironmentVariable("VIEW3D_OSBG_COVERAGE_STRICT") == QLatin1String("1");
    // 街面参考：数据集 y 下界是 -127 m 的离群值，拿它当地面会把相机埋进地里
    // （第一版就踩了这个坑，画面全空、判据全部作废）。取高度范围 20% 分位当街面，
    // 本数据集算出来约 3 m，与 SRSOrigin z=0 一致。
    const double streetY =
        steadyBounds.minimum[1] + 0.2 * (steadyBounds.maximum[1] - steadyBounds.minimum[1]);
    // 视角扫描：验收标准是"任意视角下屏内不得出现沿瓦片轮廓的空白"，只看一两个机位
    // 不足以判定（上一轮就是只看了 120 m 那张图就下了结论）。VIEW3D_OSBG_SWEEP=N
    // 时绕数据集取 N 个方位、每方位轮换三档距离，逐视角打印覆盖判据。
    const int sweepViews = qEnvironmentVariable("VIEW3D_OSBG_SWEEP").toInt();
    if (sweepViews > 0 && view) {
        const double kPi = 3.141592653589793;
        const double sweepDistances[3] = {120.0, 350.0, 800.0};
        int settleMs = qEnvironmentVariable("VIEW3D_OSBG_SETTLE_MS").toInt();
        if (settleMs <= 0)
            settleMs = 3000;
        // 机位以街面为基准，仰角固定 19°（升高 = 0.35×水平距离）：
        // 120 m 档就是用户截图那种低空穿城视角。
        int worstSweepBlank = 0;
        int worstSweepView = 0;
        for (int i = 0; i < sweepViews; ++i) {
            const double azimuth = 2.0 * kPi * i / sweepViews;
            const double distance = sweepDistances[i % 3];
            View3D::ViewportCameraState sweep = camera;
            sweep.position = {centre[0] + std::sin(azimuth) * distance,
                              streetY + 0.35 * distance,
                              centre[2] + std::cos(azimuth) * distance};
            sweep.target = {centre[0], streetY, centre[2]};
            const double dx = centre[0] - sweep.position[0];
            const double dy = streetY - sweep.position[1];
            const double dz = centre[2] - sweep.position[2];
            const double norm = std::sqrt(dx * dx + dy * dy + dz * dz);
            sweep.direction = {dx / norm, dy / norm, dz / norm};
            view->setCameraState(sweep);
            timer.restart();
            while (timer.elapsed() < settleMs) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(10);
            }
            const QImage shot =
                view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
            const CoverageStats cover = analyseCoverage(
                shot, coverRef,
                buildFootprintMask(sweep, shotSize, streetY, steadyBounds.minimum,
                                   steadyBounds.maximum));
            const QByteArray label = "sweep" + QByteArray::number(i + 1) + "_of" +
                QByteArray::number(sweepViews);
            std::cerr << "VIEW3D_OSBG_COVERAGE view=" << label.constData()
                      << " az_deg=" << int(azimuth * 180.0 / kPi)
                      << " horiz_m=" << distance
                      << " cam_y=" << sweep.position[1]
                      << " above_street=" << (sweep.position[1] - streetY);
            if (!cover.valid) {
                std::cerr << " valid=0" << std::endl;
                continue;
            }
            std::cerr << " foot_blank_permille=" << cover.footBlankPermille
                      << " foot_permille=" << cover.footPermille
                      << " blank_permille=" << cover.blankPermille
                      << " hole_permille=" << cover.holePermille
                      << " bad_cells=" << cover.badCells << "/" << cover.judgedCells
                      << " edge=" << cover.edgeMean
                      << " map=" << cover.map.constData() << std::endl;
            // 掩码只占屏幕不到 5% 时，foot_blank 是薄片的噪声，不计入最差视角。
            if (cover.footPermille >= 50 && cover.footBlankPermille > worstSweepBlank) {
                worstSweepBlank = cover.footBlankPermille;
                worstSweepView = i + 1;
            }
            if (!dumpPrefix.isEmpty())
                shot.save(dumpPrefix + QStringLiteral("_sweep%1.png").arg(i + 1));
        }
        std::cerr << "VIEW3D_OSBG_SWEEP views=" << sweepViews
                  << " worst_foot_blank_permille=" << worstSweepBlank
                  << " worst_view=" << worstSweepView << " max=" << blankMax << std::endl;
        if (coverageStrict && worstSweepBlank > blankMax)
            return fail(QStringLiteral("扫描第 %1 个视角 footprint 空白 %2‰ 超过 %3‰")
                            .arg(worstSweepView)
                            .arg(worstSweepBlank)
                            .arg(blankMax));
        view->setCameraState(camera);
        timer.restart();
        while (timer.elapsed() < 1500) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    }
    // ── 球面模式：倾斜摄影按 SRS 逐顶点烘焙上球面（VIEW3D_OSBG_GLOBE=1 时启用）──
    if (qEnvironmentVariableIsSet("VIEW3D_OSBG_GLOBE")) {
        const auto dist3 = [](const std::array<double, 3> &a,
                              const std::array<double, 3> &b) {
            const double dx = a[0] - b[0];
            const double dy = a[1] - b[1];
            const double dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        auto *layers = container->objectAs<View3D::ISceneLayerService>(
            View3D::ServiceIds::SceneLayers);
        auto *reference = container->objectAs<View3D::IGeoReference>(
            View3D::ServiceIds::GeoReference);
        if (!layers || !reference)
            return fail(QStringLiteral("球面验证缺少图层或参照系服务"));
        const View3D::LayerId tileLayer =
            layers->layerByRenderKey(QStringLiteral("tile:") + groupKey);
        if (tileLayer.isNull())
            return fail(QStringLiteral("球面验证找不到倾斜摄影图层"));
        const View3D::ILayer::ReferenceFrame flatFrame =
            layers->layerReferenceFrame(tileLayer);
        if (!flatFrame.hasMatrix)
            return fail(QStringLiteral("平面模式下倾斜摄影参照系应展示换算矩阵"));
        double originLon = 0.0;
        double originLat = 0.0;
        {
            const QRegularExpression pattern(QStringLiteral("([-0-9.]+)°,\\s*([-0-9.]+)°"));
            const QRegularExpressionMatch match = pattern.match(flatFrame.position);
            if (match.hasMatch()) {
                originLon = match.captured(1).toDouble();
                originLat = match.captured(2).toDouble();
            }
        }
        if (flatFrame.position.isEmpty())
            std::cerr << "VIEW3D_OSBG_GLOBE_WARN reason=no_geographic_origin" << std::endl;
        View3D::ViewportCameraState flatBefore;
        if (!view->cameraState(&flatBefore))
            return fail(QStringLiteral("球面切换前相机不可读"));
        std::array<double, 3> expectedEcef {};
        if (!reference->localToEcefPoint(flatBefore.position, &expectedEcef))
            return fail(QStringLiteral("相机局部坐标换算 ECEF 失败"));
        if (!layout->setGlobeModeEnabled(true))
            return fail(QStringLiteral("倾斜摄影进入球面模式失败"));
        if (!scene->globeModeEnabled() || !view->globeModeEnabled())
            return fail(QStringLiteral("球面模式没有传播到场景/视口"));
        if (layers->layerReferenceFrame(tileLayer).hasMatrix)
            return fail(QStringLiteral("球面模式下倾斜摄影仍展示换算矩阵"));
        timer.restart();
        while (timer.elapsed() < 30000 && scene->tileCount(groupKey) == 0) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        if (scene->tileCount(groupKey) == 0)
            return fail(QStringLiteral("球面模式下倾斜摄影没有重新上屏"));
        // 等瓦片数量稳定（连续 1 s）再抓图/拾取，避免把 LOD 精化当成位置错。
        int stableTiles = -1;
        qint64 stableSince = 0;
        timer.restart();
        while (timer.elapsed() < 20000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
            const int count = static_cast<int>(scene->tileCount(groupKey));
            if (count != stableTiles) {
                stableTiles = count;
                stableSince = timer.elapsed();
            } else if (timer.elapsed() - stableSince > 1000) {
                break;
            }
        }
        View3D::ViewportCameraState globeCamera;
        if (!view->cameraState(&globeCamera))
            return fail(QStringLiteral("球面模式下相机不可读"));
        const double enterError = dist3(globeCamera.position, expectedEcef);
        if (enterError > 1.0)
            return fail(QStringLiteral("倾斜摄影进入球面相机换算误差过大：%1 m")
                            .arg(enterError));
        std::array<double, 3> anchor {};
        if (!scene->globeRenderOrigin(&anchor))
            return fail(QStringLiteral("球面模式没有渲染原点"));
        const QImage globeShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (!dumpPrefix.isEmpty() && !globeShot.isNull())
            globeShot.save(dumpPrefix + QStringLiteral("_osgb_globe.png"));
        const ShotStats globeStats = analyseShot(globeShot, QImage());
        if (globeStats.distinctColours < 5)
            return fail(QStringLiteral("球面模式下倾斜摄影没有上屏：colours=%1")
                            .arg(globeStats.distinctColours));
        // 位置正确性：视线中心拾取点换算经纬度应落在 SRSOrigin 附近（城市尺度）。
        View3D::ViewportPickResult pick;
        const std::array<double, 3> pickOrigin = {
            globeCamera.position[0] - anchor[0], globeCamera.position[1] - anchor[1],
            globeCamera.position[2] - anchor[2]};
        const bool picked = scene->pickRay(pickOrigin, globeCamera.direction, &pick) &&
            pick.hasWorldPosition;
        double pickLon = 0.0;
        double pickLat = 0.0;
        double pickHeight = 0.0;
        if (picked) {
            const std::array<double, 3> ecef = {
                pick.worldPosition[0] + anchor[0], pick.worldPosition[1] + anchor[1],
                pick.worldPosition[2] + anchor[2]};
            View3D::GlobeMath::ecefToGeodetic(ecef, &pickLon, &pickLat, &pickHeight);
        }
        if (!picked)
            return fail(QStringLiteral("球面模式下中心视线没有命中倾斜摄影"));
        const double lonError = qAbs(pickLon - originLon);
        const double latError = qAbs(pickLat - originLat);
        if (!flatFrame.position.isEmpty() && (lonError > 0.1 || latError > 0.1))
            return fail(QStringLiteral("球面摆放位置与 SRSOrigin 不一致：Δlon=%1° Δlat=%2°")
                            .arg(lonError, 0, 'f', 5)
                            .arg(latError, 0, 'f', 5));
        // 拉远到数据集周边（约 1 km 高度）：直观确认成果贴在椭球表面。
        for (int step = 0; step < 10; ++step) {
            view->zoomCamera(-1.0);
            QApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        timer.restart();
        while (timer.elapsed() < 800) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        const QImage farShot =
            view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (!dumpPrefix.isEmpty() && !farShot.isNull())
            farShot.save(dumpPrefix + QStringLiteral("_osgb_globe_far.png"));
        view->setCameraState(globeCamera);
        // 退出：矩阵恢复、瓦片回平面烘焙。
        if (!layout->setGlobeModeEnabled(false))
            return fail(QStringLiteral("倾斜摄影退出球面模式失败"));
        if (scene->globeModeEnabled() || view->globeModeEnabled())
            return fail(QStringLiteral("退出球面后模式标志仍为真"));
        if (!layers->layerReferenceFrame(tileLayer).hasMatrix)
            return fail(QStringLiteral("退出球面后倾斜摄影参照系矩阵没有恢复"));
        timer.restart();
        while (timer.elapsed() < 30000 && scene->tileCount(groupKey) == 0) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        if (scene->tileCount(groupKey) == 0)
            return fail(QStringLiteral("退出球面后倾斜摄影没有回到平面烘焙"));
        // 单次写入，避免与流式线程日志交错后拆行（ctest 用这一行判通过）。
        const QString summary = QStringLiteral(
            "VIEW3D_OSGB_GLOBE_OK enter_err=%1 stable_tiles=%2 colours=%3 "
            "pick_lon=%4 pick_lat=%5 origin_lon=%6 origin_lat=%7 dlon=%8 dlat=%9 "
            "pick_height=%10")
                                    .arg(enterError)
                                    .arg(stableTiles)
                                    .arg(globeStats.distinctColours)
                                    .arg(pickLon)
                                    .arg(pickLat)
                                    .arg(originLon)
                                    .arg(originLat)
                                    .arg(lonError)
                                    .arg(latError)
                                    .arg(pickHeight);
        std::cerr << summary.toStdString() << std::endl;
    }

    // 倾斜摄影图层必须由数据插件提供右键项；用菜单卸载同时验证插件接管了图层删除，
    // 而不是只把树上的条目删掉、后台流式加载还在跑。
    auto *dock = container->objectAs<View3D::ISceneTreeDock>(
        View3D::ServiceIds::SceneTreeDock);
    QTreeWidget *sceneTree = dock ? dock->sceneTree() : nullptr;
    QTreeWidgetItem *tileItem = nullptr;
    if (sceneTree) {
        for (QTreeWidgetItemIterator iterator(sceneTree); *iterator; ++iterator) {
            if ((*iterator)->text(0).contains(QStringLiteral("倾斜摄影"))) {
                tileItem = *iterator;
                break;
            }
        }
    }
    if (!sceneTree || !tileItem)
        return fail(QStringLiteral("倾斜摄影图层未进入场景树"));
    const QPoint tileMenuPoint = sceneTreeItemMenuPoint(sceneTree, tileItem);
    const SceneTreeMenuProbe tileMenu = probeSceneTreeMenu(sceneTree, tileMenuPoint);
    QStringList tileExpected;
    tileExpected << QStringLiteral("定位到数据集") << QStringLiteral("重新加载数据集")
                 << QStringLiteral("卸载数据集");
    QString missingTileAction;
    if (!tileMenu.shown || !menuHasActions(tileMenu, tileExpected, &missingTileAction))
        return fail(QStringLiteral("倾斜摄影图层菜单缺少：%1").arg(missingTileAction));

    const View3D::LayerId layerBeforeReload =
        tileItem->data(0, Qt::UserRole + 1).toUuid();
    const SceneTreeMenuProbe reloadViaMenu = probeSceneTreeMenu(
        sceneTree, tileMenuPoint, QStringLiteral("重新加载数据集"));
    if (reloadViaMenu.triggered.isEmpty())
        return fail(QStringLiteral("倾斜摄影图层菜单无法重新加载数据集"));
    QApplication::processEvents();
    QTreeWidgetItem *reloadedItem = nullptr;
    for (QTreeWidgetItemIterator iterator(sceneTree); *iterator; ++iterator) {
        if ((*iterator)->text(0).contains(QStringLiteral("倾斜摄影"))) {
            reloadedItem = *iterator;
            break;
        }
    }
    if (!reloadedItem ||
        reloadedItem->data(0, Qt::UserRole + 1).toUuid() == layerBeforeReload)
        return fail(QStringLiteral("重新加载数据集未替换图层"));
    timer.restart();
    while (timer.elapsed() < 2000 && scene->tileCount(groupKey) == 0) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->tileCount(groupKey) == 0)
        return fail(QStringLiteral("重新加载数据集后没有瓦片"));
    tileItem = reloadedItem;
    const QPoint reloadedMenuPoint = sceneTreeItemMenuPoint(sceneTree, tileItem);

    const SceneTreeMenuProbe unloadViaMenu = probeSceneTreeMenu(
        sceneTree, reloadedMenuPoint, QStringLiteral("卸载数据集"));
    if (unloadViaMenu.triggered.isEmpty())
        return fail(QStringLiteral("倾斜摄影图层菜单无法卸载数据集"));
    std::cerr << "VIEW3D_SCENE_TREE_MENU_OK osgb=1 reload=1 unload=1\n";

    // 卸载后必须把资源全部还给场景。
    timer.restart();
    while (timer.elapsed() < 2000 && scene->registeredVertexCount() > baseVertices) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->registeredVertexCount() != baseVertices)
        return fail(QStringLiteral("卸载数据集后仍占用顶点资源"));
    int changedPixels = 0;
    QImage emptyShot;
    if (!loadedShot.isNull() && view) {
        emptyShot = view->captureImageOffscreen(shotSize, View3D::CaptureOptions());
        if (emptyShot.size() == loadedShot.size()) {
            const QImage before = loadedShot.convertToFormat(QImage::Format_ARGB32);
            const QImage after = emptyShot.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < before.height(); ++y) {
                const auto *lineBefore = reinterpret_cast<const QRgb *>(before.constScanLine(y));
                const auto *lineAfter = reinterpret_cast<const QRgb *>(after.constScanLine(y));
                for (int x = 0; x < before.width(); ++x) {
                    if (lineBefore[x] != lineAfter[x])
                        ++changedPixels;
                }
            }
        }
    }
    if (changedPixels * 100 < 4 * shotSize.width() * shotSize.height())
        return fail(QStringLiteral("倾斜摄影未真正渲染（像素变化 %1 个）").arg(changedPixels));
    const ShotStats cityStats = analyseShot(cityShot, emptyShot);
    const ShotStats streetStats = analyseShot(loadedShot, emptyShot);
    // 先验证量具：背景参考是加载前采的，空场景图是卸载后采的，两者应当逐像素相同
    // （天空渐变是屏幕空间的）。不一致就说明参考图不可信，覆盖率数字全部作废。
    int refDiff = 0;
    if (backgroundShot.size() == emptyShot.size() && !backgroundShot.isNull()) {
        const QImage a = backgroundShot.convertToFormat(QImage::Format_ARGB32);
        const QImage b = emptyShot.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < a.height(); ++y) {
            const auto *la = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            const auto *lb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x)
                refDiff += la[x] != lb[x];
        }
    }
    std::cerr << "VIEW3D_OSBG_COVERAGE view=ref_check diff_pixels=" << refDiff << std::endl;
    if (coverRef.isNull())
        coverRef = emptyShot;
    // 覆盖判据（本轮验收项）：foot_blank=footprint 内的背景占比（主判据，天空与
    // 数据之外都不计入）；hole=逃不到屏幕边的封闭空白；map 是 16×16 逐格图，
    // '-'=不判定、'.'=覆盖良好、1-8=空白十分位、'#'=全空——
    // 形状直接对应"沿瓦片轮廓的空白"，不需要截图。
    const CoverageStats cityCover =
        analyseCoverage(cityShot, coverRef, buildFootprintMask(cityCamera, shotSize, streetY,
                                                               steadyBounds.minimum,
                                                               steadyBounds.maximum));
    const CoverageStats streetCover =
        analyseCoverage(loadedShot, coverRef, buildFootprintMask(camera, shotSize, streetY,
                                                                 steadyBounds.minimum,
                                                                 steadyBounds.maximum));
    const auto reportCoverage = [&](const char* name, const CoverageStats& cover) {
        if (!cover.valid) {
            std::cerr << "VIEW3D_OSBG_COVERAGE view=" << name << " valid=0" << std::endl;
            return;
        }
        std::cerr << "VIEW3D_OSBG_COVERAGE view=" << name
                  << " foot_blank_permille=" << cover.footBlankPermille
                  << " foot_permille=" << cover.footPermille
                  << " blank_permille=" << cover.blankPermille
                  << " hole_permille=" << cover.holePermille
                  << " bad_cells=" << cover.badCells << "/" << cover.judgedCells
                  << " edge=" << cover.edgeMean
                  << " max=" << blankMax
                  << " verdict=" << (cover.footBlankPermille <= blankMax ? "ok" : "bad")
                  << " map=" << cover.map.constData() << std::endl;
    };
    reportCoverage("city", cityCover);
    reportCoverage("street", streetCover);
    if (coverageStrict && streetCover.valid && streetCover.footBlankPermille > blankMax)
        return fail(QStringLiteral("footprint 内出现空白：foot_blank_permille=%1 超过 %2")
                        .arg(streetCover.footBlankPermille)
                        .arg(blankMax));
    // 离屏渲染结果可直接存盘判读（只用于诊断，不涉及桌面截图）。
    if (!dumpPrefix.isEmpty()) {
        cityShot.save(dumpPrefix + QStringLiteral("_city.png"));
        loadedShot.save(dumpPrefix + QStringLiteral("_street.png"));
        emptyShot.save(dumpPrefix + QStringLiteral("_empty.png"));
        backgroundShot.save(dumpPrefix + QStringLiteral("_background.png"));
        std::cerr << "VIEW3D_OSBG_DUMPED path=" << dumpPrefix.toStdString() << std::endl;
    }
    std::cerr << "VIEW3D_OSBG_SHOTS city_dark=" << cityStats.darkPermille
              << " city_blackened=" << cityStats.blackened << " city_lum=" << cityStats.meanLum
              << " city_colours=" << cityStats.distinctColours
              << " street_dark=" << streetStats.darkPermille
              << " street_blackened=" << streetStats.blackened
              << " street_lum=" << streetStats.meanLum
              << " street_colours=" << streetStats.distinctColours << std::endl;
    // 通用“打开数据”入口：数据插件注册打开器，传父组时图层必须落在该组下。
    auto *layers = container->objectAs<View3D::ISceneLayerService>(
        View3D::ServiceIds::SceneLayers);
    auto *dataOpen = container->objectAs<View3D::IDataOpenRegistry>(
        View3D::ServiceIds::DataOpen);
    if (!layers || !dataOpen || dataOpen->dataOpeners().isEmpty())
        return fail(QStringLiteral("通用打开数据入口没有登记数据插件"));
    const View3D::LayerId openGroup =
        layers->addGroup(QStringLiteral("打开数据测试组"));
    if (openGroup.isNull())
        return fail(QStringLiteral("无法创建打开数据测试组"));
    View3D::IDataOpener *opener = nullptr;
    for (View3D::IDataOpener *candidate : dataOpen->dataOpeners()) {
        if (candidate &&
            candidate->displayName().contains(QStringLiteral("倾斜摄影"))) {
            opener = candidate;
            break;
        }
    }
    if (!opener)
        opener = dataOpen->dataOpeners().first();
    QString openError;
    if (!opener->openData(directory, openGroup, &openError))
        return fail(QStringLiteral("通用打开数据失败：%1").arg(openError));
    QTreeWidgetItem *openGroupItem = nullptr;
    QTreeWidgetItem *openTileItem = nullptr;
    for (QTreeWidgetItemIterator iterator(sceneTree); *iterator; ++iterator) {
        const View3D::LayerId id = (*iterator)->data(0, Qt::UserRole + 1).toUuid();
        if (id == openGroup)
            openGroupItem = *iterator;
        if ((*iterator)->text(0).contains(QStringLiteral("倾斜摄影")))
            openTileItem = *iterator;
    }
    if (!openGroupItem || !openTileItem || openTileItem->parent() != openGroupItem)
        return fail(QStringLiteral("通用打开数据未把图层放进目标组"));
    timer.restart();
    while (timer.elapsed() < 2000 && scene->tileCount(groupKey) == 0) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->tileCount(groupKey) == 0)
        return fail(QStringLiteral("通用打开数据后没有瓦片"));

    // 组内打开的图层重新加载后必须还留在原组里。
    openTileItem = nullptr;
    for (QTreeWidgetItemIterator iterator(sceneTree); *iterator; ++iterator) {
        if ((*iterator)->text(0).contains(QStringLiteral("倾斜摄影"))) {
            openTileItem = *iterator;
            break;
        }
    }
    if (!openTileItem)
        return fail(QStringLiteral("组内打开的图层不在场景树里"));
    const SceneTreeMenuProbe groupedReload = probeSceneTreeMenu(
        sceneTree, sceneTreeItemMenuPoint(sceneTree, openTileItem),
        QStringLiteral("重新加载数据集"));
    if (groupedReload.triggered.isEmpty())
        return fail(QStringLiteral("组内图层无法重新加载"));
    QApplication::processEvents();
    QTreeWidgetItem *regroupedItem = nullptr;
    for (QTreeWidgetItemIterator iterator(sceneTree); *iterator; ++iterator) {
        if ((*iterator)->text(0).contains(QStringLiteral("倾斜摄影"))) {
            regroupedItem = *iterator;
            break;
        }
    }
    if (!regroupedItem || regroupedItem->parent() != openGroupItem)
        return fail(QStringLiteral("重新加载后图层离开了原组"));
    timer.restart();
    while (timer.elapsed() < 2000 && scene->tileCount(groupKey) == 0) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->tileCount(groupKey) == 0)
        return fail(QStringLiteral("组内图层重新加载后没有瓦片"));
    if (!loader->unloadDataset(directory))
        return fail(loader->lastError());
    timer.restart();
    while (timer.elapsed() < 2000 && scene->registeredVertexCount() > baseVertices) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (scene->registeredVertexCount() != baseVertices)
        return fail(QStringLiteral("通用打开数据卸载后仍占用顶点资源"));
    layers->removeLayer(openGroup);
    std::cerr << "VIEW3D_DATA_OPEN_OK group=1\n";

    std::cerr << "VIEW3D_OSBG_SMOKE_OK roots=" << rootTiles << " refined=" << refinedTiles
              << " steady=" << steadyTiles << " steady_vertices=" << steadyVertices
              << " first_content_ms=" << firstContentMs << " refine_ms=" << refineMs
              << " near_tiles=" << nearTiles << " near_ms=" << nearMs
              << " near_m=" << nearDistance
              << " settled_tiles=" << settledNearTiles << " settled_ms=" << settledNearMs
              << " pixels_changed=" << changedPixels << " unloaded=1" << std::endl;
    return true;
}

// 无坐标系模型导入的球面路径：临时写一张 100 m 方片 OBJ（无 CRS、节点无变换），
// 走模型打开器导入 → 进入球面后会话自动重烘焙：局部原点钉在 (0°,0°) 地表，
// 顶点 +X 朝东、+Y 朝地心反向（ENU 的上）、+Z 朝南；退出球面恢复平面几何。
// VIEW3D_MODEL_GLOBE=1 时启用（不依赖外部数据）。
bool verifyModelGlobeSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!qEnvironmentVariableIsSet("VIEW3D_MODEL_GLOBE"))
        return true;
    auto *container = application.getPluginContainer();
    auto *scene = container
        ? container->objectAs<View3D::IRenderScene>(View3D::ServiceIds::Scene)
        : nullptr;
    auto *layers = container
        ? container->objectAs<View3D::ISceneLayerService>(View3D::ServiceIds::SceneLayers)
        : nullptr;
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    auto *registry = container
        ? container->objectAs<View3D::IDataOpenRegistry>(View3D::ServiceIds::DataOpen)
        : nullptr;
    if (!scene || !layers || !layout || !registry)
        return fail(QStringLiteral("球面模型验证缺少服务"));
    View3D::IDataOpener *opener = nullptr;
    for (View3D::IDataOpener *candidate : registry->dataOpeners()) {
        if (candidate && candidate->fileFilter().contains(QStringLiteral("obj"),
                                                          Qt::CaseInsensitive)) {
            opener = candidate;
            break;
        }
    }
    if (!opener)
        return fail(QStringLiteral("没有支持 OBJ 的模型打开器"));
    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return fail(QStringLiteral("无法创建临时目录"));
    const QString path = tempDir.filePath(QStringLiteral("globe_probe.obj"));
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return fail(QStringLiteral("无法写入临时 OBJ"));
        file.write("v 0 0 0\nv 100 0 0\nv 100 0 100\nv 0 0 100\n"
                   "vn 0 1 0\nf 1//1 2//1 3//1 4//1\n");
    }
    const auto waitFor = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    // 导入选项对话框是模态的：无头跑替用户点确定。
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isModal() && dialog->isVisible() &&
                dialog->windowTitle().contains(QStringLiteral("导入选项"))) {
                dialog->accept();
                return;
            }
        }
    });
    QSet<QString> beforeKeys;
    for (const QString &key : scene->modelLayerKeys())
        beforeKeys.insert(key);
    const View3D::LayerId group = layers->addGroup(QStringLiteral("球面模型探针"));
    QString openError;
    if (!opener->openData(path, group, &openError))
        return fail(QStringLiteral("模型打开器拒绝临时 OBJ：%1").arg(openError));
    QString renderKey;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 30000 && renderKey.isEmpty()) {
        waitFor(100);
        for (const QString &key : scene->modelLayerKeys()) {
            if (!beforeKeys.contains(key) && !key.startsWith(QLatin1String("tile:")))
                renderKey = key;
        }
    }
    if (renderKey.isEmpty()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("临时 OBJ 没有产生图层"));
    }
    const View3D::LayerId layer = layers->layerByRenderKey(renderKey);
    if (layer.isNull()) {
        layers->removeLayer(group);
        return fail(QStringLiteral("临时 OBJ 图层不可定位"));
    }
    // 等平面网格就绪（顶点 4 个）。
    bool flatReady = false;
    clock.restart();
    while (clock.elapsed() < 15000 && !flatReady) {
        waitFor(50);
        const auto mesh = scene->modelLayerMesh(renderKey);
        flatReady = mesh && mesh->vertices.size() >= 4;
    }
    if (!flatReady) {
        layers->removeLayer(group);
        return fail(QStringLiteral("临时 OBJ 没有几何"));
    }
    if (scene->modelLayerMesh(renderKey)->hasContentOrigin) {
        layers->removeLayer(group);
        return fail(QStringLiteral("平面模式下模型不应带球面原点"));
    }
    if (!layout->setGlobeModeEnabled(true)) {
        layers->removeLayer(group);
        return fail(QStringLiteral("模球面切换失败"));
    }
    // 会话监听器异步重烘焙：等 hasContentOrigin 出现。
    bool baked = false;
    clock.restart();
    while (clock.elapsed() < 30000 && !baked) {
        waitFor(50);
        const auto mesh = scene->modelLayerMesh(renderKey);
        baked = mesh && mesh->hasContentOrigin;
    }
    if (!baked) {
        layout->setGlobeModeEnabled(false);
        layers->removeLayer(group);
        return fail(QStringLiteral("无坐标系模型没有重烘焙到球面"));
    }
    const auto bakedMesh = scene->modelLayerMesh(renderKey);
    // 局部中心 (50,0,50) → ECEF 应为 anchor + (y,x,−z) = (6378137, 50, −50)。
    const std::array<double, 3> anchor =
        View3D::GlobeMath::geodeticToEcef(0.0, 0.0, 0.0);
    const std::array<double, 3> expectedOrigin = {anchor[0], anchor[1] + 50.0,
                                                   anchor[2] - 50.0};
    const auto dist3 = [](const std::array<double, 3> &a, const std::array<double, 3> &b) {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        const double dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const double originError = dist3(bakedMesh->contentOrigin, expectedOrigin);
    if (originError > 5.0) {
        layout->setGlobeModeEnabled(false);
        layers->removeLayer(group);
        return fail(QStringLiteral("无坐标系模型的球面原点不在 (0°,0°) 地表：%1 m")
                        .arg(originError));
    }
    // 退出球面：无缓存图层直接换回原网格。
    bool restored = false;
    if (layout->setGlobeModeEnabled(false)) {
        clock.restart();
        while (clock.elapsed() < 15000 && !restored) {
            waitFor(50);
            const auto mesh = scene->modelLayerMesh(renderKey);
            restored = mesh && !mesh->hasContentOrigin;
        }
    }
    layers->removeLayer(group);
    if (!restored)
        return fail(QStringLiteral("退出球面后无坐标系模型没有恢复平面几何"));
    // 单次写入，避免与后台日志交错后拆行（ctest 用这一行判通过）。
    const QString summary =
        QStringLiteral("VIEW3D_MODEL_GLOBE_OK origin_error=%1 anchor=%2,%3,%4")
            .arg(originError)
            .arg(anchor[0])
            .arg(anchor[1])
            .arg(anchor[2]);
    std::cerr << summary.toStdString() << std::endl;
    return true;
}

bool verifySceneTreeSmoke(RF::PluginEngine::IApplication &application, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *container = application.getPluginContainer();
    auto *dock = container
        ? container->objectAs<View3D::ISceneTreeDock>(
              View3D::ServiceIds::SceneTreeDock)
        : nullptr;
    if (!dock) {
        std::cerr << "VIEW3D_SCENE_TREE_SKIPPED framework_only=1\n";
        return true;
    }
    auto *tree = dock->sceneTree();
    auto *root = dock->sceneRootItem();
    auto *filter = dock->sceneTypeFilter();
    if (!tree || !root || !filter)
        return fail(QStringLiteral("场景树控件不可用"));
    if (tree->topLevelItemCount() != 1)
        return fail(QStringLiteral("场景根节点必须唯一"));
    if (tree->topLevelItem(0) != root)
        return fail(QStringLiteral("场景根节点未挂到树顶"));
    if (root->text(0) != QStringLiteral("场景"))
        return fail(QStringLiteral("场景根节点名称错误"));
    if (!root->toolTip(0).contains(QStringLiteral("当前视图")))
        return fail(QStringLiteral("场景根节点未关联当前视图"));
    if (root->icon(0).isNull())
        return fail(QStringLiteral("场景根节点缺少图标"));
    if (tree->contextMenuPolicy() != Qt::CustomContextMenu)
        return fail(QStringLiteral("场景树未启用自定义右键菜单"));

    QStringList expectedFilters;
    expectedFilters << QStringLiteral("全部类型") << QStringLiteral("组图层")
                    << QStringLiteral("图层");
    if (filter->count() != expectedFilters.size())
        return fail(QStringLiteral("场景树类型筛选项数量错误"));
    for (int index = 0; index < expectedFilters.size(); ++index) {
        if (filter->itemText(index) != expectedFilters.at(index))
            return fail(QStringLiteral("场景树类型筛选项错误：%1")
                            .arg(filter->itemText(index)));
    }

    auto *layout = container->objectAs<View3D::IViewportLayout>(
        View3D::ServiceIds::ViewportLayout);
    auto *primaryView = layout ? layout->primaryView() : nullptr;
    if (primaryView && primaryView->ready() && primaryView->widget() &&
        primaryView->widget()->width() > 0 && primaryView->widget()->height() > 0) {
        primaryView->refreshRenderResolution();
        QApplication::processEvents();
        QWidget *viewWidget = primaryView->widget();
        const qreal dpr = viewWidget->devicePixelRatioF();
        const QSize expected(qMax(1, qRound(viewWidget->width() * dpr)),
                             qMax(1, qRound(viewWidget->height() * dpr)));
        const QSize actual = primaryView->renderPixelSize();
        if (actual != expected) {
            return fail(QStringLiteral("渲染分辨率未同步：期望 %1x%2，实际 %3x%4")
                            .arg(expected.width())
                            .arg(expected.height())
                            .arg(actual.width())
                            .arg(actual.height()));
        }
        std::cerr << "VIEW3D_RENDER_RESOLUTION_OK widget=" << viewWidget->width() << "x"
                  << viewWidget->height() << " pixel=" << actual.width() << "x"
                  << actual.height() << '\n';
    } else {
        std::cerr << "VIEW3D_RENDER_RESOLUTION_SKIPPED reason=no_view\n";
    }

    int items = 0;
    bool hasLegacyNode = false;
    bool hasDemo = false;
    for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator) {
        QTreeWidgetItem *item = *iterator;
        ++items;
        if (item->data(0, Qt::UserRole + 2).isValid())
            hasLegacyNode = true;
        if (item->text(0).contains(QStringLiteral("演示")))
            hasDemo = true;
        if (item != root && item->data(0, Qt::UserRole + 1).toUuid().isNull())
            return fail(QStringLiteral("场景树包含非图层项：%1").arg(item->text(0)));
    }
    if (hasLegacyNode)
        return fail(QStringLiteral("场景树仍包含模型节点字段"));
    if (hasDemo)
        return fail(QStringLiteral("场景树仍包含演示内容"));

    auto *layers = container->objectAs<View3D::ISceneLayerService>(
        View3D::ServiceIds::SceneLayers);
    if (layers) {
        const auto mesh = makeSmokeTriangleMesh();
        const View3D::LayerId groupId = layers->addGroup(QStringLiteral("右键菜单测试组"));
        const View3D::LayerId layerA = layers->addModelLayer(
            QStringLiteral("右键菜单图层A"), QStringLiteral("smoke:menu_a"), mesh, groupId);
        const View3D::LayerId layerB = layers->addModelLayer(
            QStringLiteral("右键菜单图层B"), QStringLiteral("smoke:menu_b"), mesh, groupId);
        if (groupId.isNull() || layerA.isNull() || layerB.isNull())
            return fail(QStringLiteral("无法登记右键菜单测试图层"));
        QApplication::processEvents();

        const auto findItem = [tree](const View3D::LayerId &layerId) -> QTreeWidgetItem * {
            for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator) {
                if ((*iterator)->data(0, Qt::UserRole + 1).toUuid() == layerId)
                    return *iterator;
            }
            return nullptr;
        };
        QTreeWidgetItem *groupItem = findItem(groupId);
        QTreeWidgetItem *itemA = findItem(layerA);
        QTreeWidgetItem *itemB = findItem(layerB);
        if (!groupItem || !itemA || !itemB)
            return fail(QStringLiteral("右键菜单测试图层未进入场景树"));
        if (groupItem->icon(0).isNull() || itemA->icon(0).isNull() ||
            itemB->icon(0).isNull()) {
            return fail(QStringLiteral("场景树图层缺少类型图标"));
        }
        if (groupItem->icon(0).cacheKey() == itemA->icon(0).cacheKey())
            return fail(QStringLiteral("组和模型图层使用了同一图标"));

        if (tree->dragDropMode() != QAbstractItemView::InternalMove)
            return fail(QStringLiteral("场景树拖拽模式不是内部移动"));
        const QPoint rootDropPoint = sceneTreeItemMenuPoint(tree, root);
        if (!dock->sceneTreeDropAllowed(rootDropPoint))
            return fail(QStringLiteral("场景根不允许接收图层"));
        const QPoint layerDropPoint = sceneTreeItemMenuPoint(tree, itemA);
        if (!dock->sceneTreeDropAllowed(layerDropPoint))
            return fail(QStringLiteral("场景根内部的图层落点被误拒"));
        tree->resize(420, 640);
        QApplication::processEvents();
        int lastItemBottom = 0;
        for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator)
            lastItemBottom =
                qMax(lastItemBottom, tree->visualItemRect(*iterator).bottom());
        const int viewportHeight = tree->viewport()->height();
        const QPoint blankPoint(10, qMin(viewportHeight - 2, lastItemBottom + 12));
        if (viewportHeight < lastItemBottom + 8 || tree->itemAt(blankPoint))
            return fail(QStringLiteral("场景树没有可用的空白落点"));
        if (dock->sceneTreeDropAllowed(blankPoint))
            return fail(QStringLiteral("空白处允许把图层拖成场景同级"));
        std::cerr << "VIEW3D_SCENE_TREE_DRAG_OK root=1 blank=1\n";

        QString missing;
        auto *dataOpen = container->objectAs<View3D::IDataOpenRegistry>(
            View3D::ServiceIds::DataOpen);
        const bool expectOpenData = dataOpen && !dataOpen->dataOpeners().isEmpty();
        const bool hasOsgb = container->objectAs<View3D::IOsbLoader>(
            View3D::ServiceIds::OsbLoader) != nullptr;
        if (hasOsgb && !expectOpenData)
            return fail(QStringLiteral("倾斜摄影插件已加载但没有登记打开数据入口"));
        QStringList rootExpected;
        if (expectOpenData)
            rootExpected << QStringLiteral("打开数据…");
        // 在线底图的入口在场景树右键菜单，不在 Ribbon。
        const bool hasImagery =
            container->objectAs<View3D::IImageryService>(View3D::ServiceIds::Imagery) != nullptr;
        if (hasImagery)
            rootExpected << QStringLiteral("添加在线底图…");
        rootExpected << QStringLiteral("新建图层组") << QStringLiteral("全部显示")
                     << QStringLiteral("全部隐藏") << QStringLiteral("聚焦全部可见")
                     << QStringLiteral("清空全部图层");
        const SceneTreeMenuProbe rootMenu =
            probeSceneTreeMenu(tree, sceneTreeItemMenuPoint(tree, root));
        if (!rootMenu.shown)
            return fail(QStringLiteral("场景根右键菜单未弹出"));
        if (!menuHasActions(rootMenu, rootExpected, &missing))
            return fail(QStringLiteral("场景根菜单缺少：%1").arg(missing));
        if (rootMenu.iconActions < 4)
            return fail(QStringLiteral("场景根菜单图标不足：%1").arg(rootMenu.iconActions));
        if (rootMenu.actions.contains(QStringLiteral("打开倾斜摄影数据集…")))
            return fail(QStringLiteral("场景根仍在显示倾斜摄影专用打开入口"));

        const SceneTreeMenuProbe addGroupMenu =
            probeSceneTreeMenu(tree, sceneTreeItemMenuPoint(tree, root),
                               QStringLiteral("新建图层组"));
        if (addGroupMenu.triggered.isEmpty())
            return fail(QStringLiteral("场景根菜单无法新建图层组"));
        bool createdGroup = false;
        for (int index = 0; index < root->childCount(); ++index) {
            if (root->child(index)->text(0).startsWith(QStringLiteral("新建图层组")))
                createdGroup = true;
        }
        if (!createdGroup)
            return fail(QStringLiteral("新建图层组没有出现在场景树"));

        QStringList groupExpected;
        groupExpected << QStringLiteral("新建子图层组");
        if (expectOpenData)
            groupExpected << QStringLiteral("打开数据到本组…");
        if (hasImagery)
            groupExpected << QStringLiteral("添加在线底图到本组…");
        groupExpected << QStringLiteral("组内全部显示") << QStringLiteral("组内全部隐藏")
                      << QStringLiteral("清空组内图层") << QStringLiteral("删除图层组");
        const SceneTreeMenuProbe groupMenu =
            probeSceneTreeMenu(tree, sceneTreeItemMenuPoint(tree, groupItem));
        if (!groupMenu.shown)
            return fail(QStringLiteral("组图层右键菜单未弹出"));
        if (!menuHasActions(groupMenu, groupExpected, &missing))
            return fail(QStringLiteral("组图层菜单缺少：%1").arg(missing));
        if (groupMenu.iconActions < 4)
            return fail(QStringLiteral("组图层菜单图标不足：%1").arg(groupMenu.iconActions));

        const SceneTreeMenuProbe hideGroup = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, groupItem), QStringLiteral("组内全部隐藏"));
        if (hideGroup.triggered.isEmpty() || itemA->checkState(0) != Qt::Unchecked ||
            itemB->checkState(0) != Qt::Unchecked)
            return fail(QStringLiteral("组内全部隐藏未生效"));
        const SceneTreeMenuProbe showGroup = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, groupItem), QStringLiteral("组内全部显示"));
        if (showGroup.triggered.isEmpty() || itemA->checkState(0) != Qt::Checked ||
            itemB->checkState(0) != Qt::Checked)
            return fail(QStringLiteral("组内全部显示未生效"));

        QStringList layerExpected;
        layerExpected << QStringLiteral("定位到当前图层") << QStringLiteral("删除图层");
        tree->clearSelection();
        tree->setCurrentItem(itemA);
        itemA->setSelected(true);
        QApplication::processEvents();
        const SceneTreeMenuProbe layerMenu =
            probeSceneTreeMenu(tree, sceneTreeItemMenuPoint(tree, itemA));
        if (!layerMenu.shown)
            return fail(QStringLiteral("图层右键菜单未弹出"));
        if (!menuHasActions(layerMenu, layerExpected, &missing))
            return fail(QStringLiteral("图层菜单缺少：%1").arg(missing));
        if (layerMenu.iconActions < 2)
            return fail(QStringLiteral("图层菜单图标不足：%1").arg(layerMenu.iconActions));

        tree->clearSelection();
        tree->setCurrentItem(itemA);
        itemA->setSelected(true);
        itemB->setSelected(true);
        QApplication::processEvents();
        QStringList batchExpected;
        batchExpected << QStringLiteral("批量显示") << QStringLiteral("批量隐藏")
                      << QStringLiteral("定位到当前图层") << QStringLiteral("删除图层");
        const SceneTreeMenuProbe batchMenu =
            probeSceneTreeMenu(tree, sceneTreeItemMenuPoint(tree, itemA));
        if (!batchMenu.shown)
            return fail(QStringLiteral("多选右键菜单未弹出"));
        if (!menuHasActions(batchMenu, batchExpected, &missing))
            return fail(QStringLiteral("多选菜单缺少：%1").arg(missing));
        if (batchMenu.iconActions < 4)
            return fail(QStringLiteral("多选菜单图标不足：%1").arg(batchMenu.iconActions));

        const SceneTreeMenuProbe batchHide = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, itemA), QStringLiteral("批量隐藏"));
        if (batchHide.triggered.isEmpty() || itemA->checkState(0) != Qt::Unchecked ||
            itemB->checkState(0) != Qt::Unchecked)
            return fail(QStringLiteral("批量隐藏未生效"));
        const SceneTreeMenuProbe batchShow = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, itemA), QStringLiteral("批量显示"));
        if (batchShow.triggered.isEmpty() || itemA->checkState(0) != Qt::Checked ||
            itemB->checkState(0) != Qt::Checked)
            return fail(QStringLiteral("批量显示未生效"));

        const SceneTreeMenuProbe deleteLayer = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, itemA), QStringLiteral("删除图层"));
        if (deleteLayer.triggered.isEmpty() || findItem(layerA))
            return fail(QStringLiteral("右键删除图层未生效"));

        groupItem = findItem(groupId);
        if (!groupItem)
            return fail(QStringLiteral("删除图层后组图层消失"));
        const SceneTreeMenuProbe deleteGroup = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, groupItem), QStringLiteral("删除图层组"));
        if (deleteGroup.triggered.isEmpty() || findItem(groupId) || findItem(layerB))
            return fail(QStringLiteral("删除图层组未级联清理子图层"));

        const SceneTreeMenuProbe clearAll = probeSceneTreeMenu(
            tree, sceneTreeItemMenuPoint(tree, root), QStringLiteral("清空全部图层"));
        if (clearAll.triggered.isEmpty() || root->childCount() != 0)
            return fail(QStringLiteral("清空全部图层未生效"));
        std::cerr << "VIEW3D_SCENE_TREE_MENU_OK root=1 group=1 layer=1 batch=1 delete=1\n";
    }

    std::cerr << "VIEW3D_SCENE_TREE_OK scenes=1 items=" << items << " nodes=0\n";
    return true;
}
} // namespace

bool verifyMeasurementDockClampSmoke(PluginUiHost &uiHost,
                                     ads::CDockManager *dockManager,
                                     QMainWindow &window, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto *dock = uiHost.dockWidget(QStringLiteral("dw.measurements"));
    if (!dock || !dockManager) {
        std::cerr << "VIEW3D_DOCK_CLAMP_SKIPPED reason=plugin_absent\n";
        return true;
    }
    auto *sceneDock = uiHost.dockWidget(QStringLiteral("dw.scene"));
    const int leftWidthBefore =
        sceneDock && sceneDock->dockAreaWidget() ? sceneDock->dockAreaWidget()->width() : -1;
    dock->toggleView(true);
    dockManager->addDockWidget(ads::RightDockWidgetArea, dock);
    QApplication::processEvents();
    const int leftWidthAfterMove =
        sceneDock && sceneDock->dockAreaWidget() ? sceneDock->dockAreaWidget()->width() : -1;
    std::cerr << "VIEW3D_DOCK_MOVE left_before=" << leftWidthBefore
              << " left_after=" << leftWidthAfterMove << "\n";
    auto *area = dock->dockAreaWidget();
    if (!area) {
        dock->toggleView(false);
        return fail(QStringLiteral("测量结果面板未停靠到可用区域"));
    }
    QWidget *splitterChild = area;
    QSplitter *splitter = nullptr;
    while (splitterChild && splitterChild->parentWidget()) {
        QWidget *parent = splitterChild->parentWidget();
        if (auto *candidate = qobject_cast<QSplitter *>(parent)) {
            splitter = candidate;
            break;
        }
        splitterChild = parent;
    }
    if (!splitter) {
        dock->toggleView(false);
        std::cerr << "VIEW3D_DOCK_CLAMP_SKIPPED reason=no_splitter\n";
        return true;
    }
    const int index = splitter->indexOf(splitterChild);
    QList<int> sizes = splitter->sizes();
    if (index < 0 || sizes.size() <= index) {
        dock->toggleView(false);
        return fail(QStringLiteral("测量结果面板所在分隔条不可用"));
    }
    int sceneIndex = -1;
    if (auto *sceneDock = uiHost.dockWidget(QStringLiteral("dw.scene"))) {
        QWidget *sceneChild = sceneDock->dockAreaWidget();
        QSplitter *sceneSplitter = nullptr;
        while (sceneChild && sceneChild->parentWidget()) {
            QWidget *parent = sceneChild->parentWidget();
            if (auto *candidate = qobject_cast<QSplitter *>(parent)) {
                sceneSplitter = candidate;
                break;
            }
            sceneChild = parent;
        }
        if (sceneSplitter == splitter)
            sceneIndex = splitter->indexOf(sceneChild);
    }
    if (sceneIndex >= 0) {
        const QList<int> originalSizes = splitter->sizes();
        const int sceneBaseline = originalSizes.value(sceneIndex, 0);
        if (sceneBaseline > 40) {
            QList<int> shrunkSizes = originalSizes;
            shrunkSizes[sceneIndex] = qMax(20, sceneBaseline / 3);
            splitter->setSizes(shrunkSizes);
            QApplication::processEvents();
            View3DApplication::restoreSceneDockWidth(sceneDock, sceneBaseline, &window);
            QApplication::processEvents();
            const int sceneRestored = splitter->sizes().value(sceneIndex, 0);
            if (sceneRestored != sceneBaseline) {
                dock->toggleView(false);
                return fail(QStringLiteral("左侧场景树宽度恢复失败"));
            }
            std::cerr << "VIEW3D_DOCK_KEEP_LEFT_OK baseline=" << sceneBaseline
                      << " restored=" << sceneRestored << "\n";
        }
    }
    for (int &size : sizes)
        size = qMax(80, size);
    sizes[index] = 4000;
    splitter->setSizes(sizes);
    QApplication::processEvents();
    const int before = splitter->sizes().value(index, 0);
    const int sceneBefore =
        sceneIndex >= 0 ? splitter->sizes().value(sceneIndex, 0) : -1;
    const int limit = splitter->orientation() == Qt::Horizontal
        ? qBound(360, window.width() / 3, 560)
        : qBound(240, window.height() / 3, 420);
    if (before <= limit) {
        dock->toggleView(false);
        std::cerr << "VIEW3D_DOCK_CLAMP_SKIPPED reason=hidden_layout before=" << before
                  << " limit=" << limit << "\n";
        return true;
    }
    View3DApplication::clampMeasurementDock(dock, &window);
    QApplication::processEvents();
    const int after = splitter->sizes().value(index, 0);
    const int sceneAfter =
        sceneIndex >= 0 ? splitter->sizes().value(sceneIndex, 0) : -1;
    dock->toggleView(false);
    if (after > limit) {
        std::cerr << "VIEW3D_DOCK_CLAMP_FAILED before=" << before << " after=" << after
                  << " limit=" << limit << "\n";
        return fail(QStringLiteral("测量结果面板尺寸钳制未生效"));
    }
    if (sceneIndex >= 0 && sceneAfter != sceneBefore) {
        std::cerr << "VIEW3D_DOCK_CLAMP_SIDE_CHANGED before=" << sceneBefore
                  << " after=" << sceneAfter << "\n";
        return fail(QStringLiteral("钳制右侧测量面板时改变了左侧场景树宽度"));
    }
    std::cerr << "VIEW3D_DOCK_CLAMP_OK before=" << before << " after=" << after
              << " limit=" << limit << " left=" << sceneBefore << "\n";
    std::cerr << "VIEW3D_DOCK_CLAMP_SIDE_OK left_before=" << sceneBefore
              << " left_after=" << sceneAfter << "\n";
    return true;
}

// 无边框窗口贴边操作的回归：缩放带内的双击不能再把窗口最大化，标题栏中部的双击
// 和贴边拖拽缩放本身都得照常工作。
bool verifyEdgeDoubleClickSmoke(SARibbonMainWindow &window, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const int band = View3DApplication::FramelessEdgeFilter::edgeBandWidth(&window);
    const int titleHeight = window.ribbonBar() ? window.ribbonBar()->titleBarHeight() : -1;
    if (band <= 0 || titleHeight <= band + 2) {
        std::cerr << "VIEW3D_EDGE_GUARD_SKIPPED band=" << band
                  << " title_height=" << titleHeight << "\n";
        return true;
    }
    const bool visibleBefore = window.isVisible();
    const Qt::WindowStates stateBefore = window.windowState();
    window.setWindowState(Qt::WindowNoState);
    QApplication::processEvents();
    const QRect pristine = window.frameGeometry();
    const auto restoreWindow = [&window, visibleBefore, stateBefore, pristine] {
        window.setWindowState(Qt::WindowNoState);
        window.setGeometry(pristine);
        window.setWindowState(stateBefore);
        if (!visibleBefore)
            window.hide();
    };

    const auto sendMouse = [&window](QEvent::Type type, const QPoint &localPos,
                                     Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPoint global = window.frameGeometry().topLeft() + localPos;
        QMouseEvent event(type, QPointF(localPos), QPointF(global), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    const auto doubleClick = [&sendMouse](const QPoint &localPos) {
        sendMouse(QEvent::MouseButtonDblClick, localPos, Qt::LeftButton, Qt::LeftButton);
    };

    // 守卫不能顺手把贴边缩放也吞掉：按住上边缘往下拖 40px，顶部要跟着鼠标走。
    // 这一步必须赶在窗口被真显示之前做：show 之后 Qt 会把框架边记账进来，量到的
    // top/height 就不再是无边框助手按 frameGeometry 算的那套坐标了。
    int resizeFrom = -1;
    int resizeTo = -1;
    if (window.isMaximized()) {
        std::cerr << "VIEW3D_EDGE_DRAG_SKIPPED reason=window_maximized\n";
    } else {
        const QRect grabBefore = window.frameGeometry();
        const QPoint grab(grabBefore.width() / 2, band / 2);
        const QPoint dragged = grab + QPoint(0, 40);
        sendMouse(QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, dragged, Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, dragged, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
        const QRect grabbed = window.frameGeometry();
        resizeFrom = grabBefore.height();
        resizeTo = grabbed.height();
        window.setGeometry(pristine);
        if (grabbed.top() <= grabBefore.top() || grabbed.height() >= grabBefore.height() - 10) {
            return fail(QStringLiteral("上边缘拖拽缩放未生效：top %1->%2 height %3->%4")
                            .arg(grabBefore.top())
                            .arg(grabbed.top())
                            .arg(grabBefore.height())
                            .arg(grabbed.height()));
        }
    }

    // 右上角的按钮栏紧贴窗口顶边与右边：贴在角上的按下如果落到按钮上，缩放带就被按钮
    // 吃掉，顺手还会把最大化/关闭点着。这里按全局坐标把事件投给按钮，检查它是否被接管。
    const auto sendAt = [](QWidget *target, QEvent::Type type, const QPoint &global,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPoint local = target->mapFromGlobal(global);
        QMouseEvent event(type, QPointF(local), QPointF(global), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    int cornerFrom = -1;
    int cornerTo = -1;
    auto *maxButton = window.findChild<QToolButton *>(QStringLiteral("SAMaximizeWindowButton"));
    if (!maxButton || maxButton->width() < 10 || maxButton->height() < 10) {
        std::cerr << "VIEW3D_EDGE_BUTTON_SKIPPED reason=button_not_laid_out\n";
    } else {
        // 对照：缩放带之外按下按钮，必须真的收到事件（isDown 变 true），否则下面的
        // "按钮没按下"什么都证明不了。只按下不抬手，避免真把窗口最大化/关掉。
        const QPoint centreGlobal = maxButton->mapToGlobal(maxButton->rect().center());
        sendAt(maxButton, QEvent::MouseButtonPress, centreGlobal, Qt::LeftButton, Qt::LeftButton);
        const bool controlDown = maxButton->isDown();
        sendAt(maxButton, QEvent::MouseButtonRelease, centreGlobal + QPoint(0, 4 * maxButton->height()),
               Qt::LeftButton, Qt::NoButton);
        if (!controlDown) {
            std::cerr << "VIEW3D_EDGE_BUTTON_SKIPPED reason=button_no_press\n";
        } else {
            const QRect cornerBefore = window.frameGeometry();
            const QPoint corner(cornerBefore.right() - 1, cornerBefore.top() + band / 2);
            const QPoint dragged = corner + QPoint(0, 40);
            sendAt(maxButton, QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
            const bool buttonSwallowed = !maxButton->isDown();
            sendAt(maxButton, QEvent::MouseMove, dragged, Qt::NoButton, Qt::LeftButton);
            sendAt(maxButton, QEvent::MouseButtonRelease, dragged, Qt::LeftButton, Qt::NoButton);
            QApplication::processEvents();
            const QRect cornerAfter = window.frameGeometry();
            cornerFrom = cornerBefore.height();
            cornerTo = cornerAfter.height();
            window.setGeometry(pristine);
            if (!buttonSwallowed) {
                return fail(QStringLiteral("右上角缩放带内的按下仍被系统按钮吞掉，没能进入缩放"));
            }
            if (cornerAfter.top() <= cornerBefore.top()
                || cornerAfter.height() >= cornerBefore.height() - 10) {
                return fail(QStringLiteral("右上角缩放带未被无边框助手接管：top %1->%2 height %3->%4")
                                .arg(cornerBefore.top())
                                .arg(cornerAfter.top())
                                .arg(cornerBefore.height())
                                .arg(cornerAfter.height()));
            }
        }
    }

    // 症状本身：贴住上边缘、左上角快速再按一次（Windows 报成双击）以前会直接最大化。
    const QPoint topEdge(window.frameGeometry().width() / 2, band / 2);
    doubleClick(topEdge);
    const bool topEdgeMaximized = window.isMaximized();
    const QPoint topLeftCorner(band / 2, band / 2);
    doubleClick(topLeftCorner);
    const bool cornerMaximized = window.isMaximized();
    if (topEdgeMaximized || cornerMaximized) {
        restoreWindow();
        return fail(QStringLiteral("边缘双击仍触发最大化 band=%1 top_y=%2 corner=(%3,%4)")
                        .arg(band)
                        .arg(topEdge.y())
                        .arg(topLeftCorner.x())
                        .arg(topLeftCorner.y()));
    }

    // 对照组：缩放带之外的标题栏双击必须仍然最大化——它证明合成事件确实走到了
    // SAFramelessHelper，上面的"没最大化"才有意义。放最后做，因为它会把窗口真显示出来。
    const int titleY = qMax(band + 2, titleHeight / 2);
    doubleClick(QPoint(window.frameGeometry().width() / 2, titleY));
    const bool titleMaximized = window.isMaximized();
    restoreWindow();
    if (!titleMaximized) {
        return fail(QStringLiteral("标题栏中部双击未触发最大化，无边框助手没收到合成事件 y=%1 band=%2")
                        .arg(titleY)
                        .arg(band));
    }

    std::cerr << "VIEW3D_EDGE_GUARD_OK band=" << band << " title_height=" << titleHeight
              << " edge_maximized=0 corner_maximized=0 title_maximized=1 resize="
              << resizeFrom << "->" << resizeTo << " corner_resize=" << cornerFrom << "->"
              << cornerTo << "\n";
    return true;
    return true;
}

bool runHostSmokeChecks(ThemeController &theme, SARibbonMainWindow &window,
                        PluginUiHost &uiHost, ads::CDockManager *dockManager,
                        RF::PluginEngine::IApplication &application,
                        const QList<View3D::RibbonPageLayout> &activePages,
                        const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
                        const QList<View3D::DockLayoutEntry> &activeDocks,
                        const QList<View3D::ContentsViewLayoutEntry> &activeContentsViews,
                        const QString &configPath, const QString &settingsPath,
                        QString *error)
{
    bool expectMinimizeButton = false;
    QString minimizeButtonTip;
    for (const View3D::RibbonGroupLayout::Item &item : rightButtons) {
        if (item.name == QStringLiteral("cmd.ribbon.minimize")) {
            expectMinimizeButton = true;
            minimizeButtonTip = item.hint;
            break;
        }
    }
    // layout_state.ini 会把用户手动切过的视图模式（二维/球面）带到下一次启动；
    // 无头断言以平面三维为基线，先统一复位，避免用户状态把用例带偏。
    {
        auto *layout = application.getPluginContainer()
            ? application.getPluginContainer()->objectAs<View3D::IViewportLayout>(
                  View3D::ServiceIds::ViewportLayout)
            : nullptr;
        if (layout) {
            if (layout->globeModeEnabled())
                layout->setGlobeModeEnabled(false);
            if (layout->planarMode())
                layout->setPlanarMode(false);
        }
    }
    if (!verifyThemeSmoke(theme, window, uiHost, activePages, settingsPath, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_THEME_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyRibbonLayoutSmoke(theme, window, uiHost, settingsPath,
                                 expectMinimizeButton, minimizeButtonTip, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_RIBBON_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyIconsSmoke(uiHost, activePages, rightButtons, activeDocks,
                          activeContentsViews, configPath, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_ICONS_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifySceneContentSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_SCENE_CONTENT_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifySceneTreeSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_SCENE_TREE_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyPluginEngineAssemblySmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_PLUGIN_ENGINE_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyConfiguredCommandsSmoke(uiHost, activePages, rightButtons, activeDocks,
                                       error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_CONFIG_UI_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyPluginManagerSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_PLUGIN_MANAGER_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyViewportLayoutSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_VIEWPORT_LAYOUT_FAILED "
                      << error->toStdString() << '\n';
        return false;
    }
    if (!verifyPlanarModeSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_PLANAR_MODE_FAILED "
                      << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyGeoWiringSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_GEO_WIRING_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyViewCullSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_VIEW_CULL_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyMeasureSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_MEASURE_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyInspectorAndLogSmoke(application, uiHost, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_INSPECTOR_LOG_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyModelImportSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_MODEL_IMPORT_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyCrsSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_CRS_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyDynamicProjectionSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_CRS_PROJ_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyViewportCoordinateReadout(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_VP_COORD_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyCrsLibrarySmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_CRS_LIB_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyFeatureTableSmoke(uiHost, application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_FEATURE_TABLE_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyOsgbDatasetSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_OSBG_SMOKE_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyThreeDsDatasetSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_3DS_DATASET_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyModelFileSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_MODEL_FILE_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyTilesetDatasetSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_TILES_SMOKE_FAILED " << error->toStdString() << std::endl;
        return false;
    }
    if (!verifyModelGlobeSmoke(application, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_MODEL_GLOBE_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyMeasurementDockClampSmoke(uiHost, dockManager, window, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_DOCK_CLAMP_FAILED " << error->toStdString() << '\n';
        return false;
    }
    if (!verifyEdgeDoubleClickSmoke(window, error)) {
        if (error && !error->isEmpty())
            std::cerr << "VIEW3D_EDGE_GUARD_FAILED " << error->toStdString() << '\n';
        return false;
    }
    return true;
}

void startSmokeWait(QApplication &application, RF::PluginEngine::IApplication &runtime)
{
    // 无头冒烟：不显示窗口，等当前视图的 Ogre 场景初始化完成后退出。
    // 框架-only 构建没有 view.document_main，启动完成即可退出。
    auto *container = runtime.getPluginContainer();
    auto *layout = container
        ? container->objectAs<View3D::IViewportLayout>(View3D::ServiceIds::ViewportLayout)
        : nullptr;
    if (!layout) {
        std::cout << "VIEW3D_SMOKE_OK framework_only=1 vertices=0" << std::endl;
        QTimer::singleShot(0, &application, [&application] { application.quit(); });
        return;
    }
    auto *smokeTimer = new QTimer(&application);
    auto elapsed = std::make_shared<int>(0);
    smokeTimer->setInterval(50);
    QObject::connect(smokeTimer, &QTimer::timeout, &application,
                     [&, smokeTimer, elapsed] {
        *elapsed += 50;
        std::uint64_t vertices = 0;
        bool sceneReady = false;
        auto *viewportLayout = runtime.getPluginContainer()
            ? runtime.getPluginContainer()->objectAs<View3D::IViewportLayout>(
                  View3D::ServiceIds::ViewportLayout)
            : nullptr;
        auto *viewport = viewportLayout ? viewportLayout->primaryView() : nullptr;
        if (viewport) {
            sceneReady = viewport->ready();
            vertices = viewport->scene() ? viewport->scene()->registeredVertexCount() : 0;
        }
        if (*elapsed % 250 == 0 || sceneReady)
            std::cerr << "VIEW3D_SMOKE t=" << *elapsed
                      << " scene_ready=" << sceneReady
                      << " vertices=" << vertices << '\n';
        if (sceneReady) {
            std::cout << "VIEW3D_SMOKE_OK scene_ready=1 vertices=" << vertices
                      << std::endl;
            smokeTimer->stop();
            application.quit();
        } else if (*elapsed >= 15000) {
            std::cerr << "VIEW3D_SMOKE_FAILED scene_ready=0\n";
            smokeTimer->stop();
            application.exit(41);
        }
    });
    smokeTimer->start();
}

} // namespace View3DApplication
