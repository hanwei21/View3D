#include "ThemeController.h"

#include <IApplication.h>
#include <IChoiceCommand.h>
#include <ICommand.h>
#include <PluginFactoryRegistry.h>

#include <QApplication>
#include <QColor>
#include <QHash>
#include <QSettings>

#include <SARibbonBar.h>
#include <SARibbonMainWindow.h>

#include <optional>
#include <utility>

namespace View3DApplication {
namespace {

QString normalizeKey(const QString& value)
{
    QString key = value.simplified().toLower();
    key.replace(QLatin1Char('_'), QLatin1Char('-'));
    key.replace(QLatin1Char(' '), QLatin1Char('-'));
    return key;
}

std::optional<SARibbonTheme> lightThemeForKey(const QString& key)
{
    if (key.isEmpty() || key == QStringLiteral("light") ||
        key == QStringLiteral("office2013") || key == QStringLiteral("office-2013")) {
        return SARibbonTheme::RibbonThemeOffice2013;
    }
    if (key == QStringLiteral("office2016-blue") || key == QStringLiteral("office2016")) {
        return SARibbonTheme::RibbonThemeOffice2016Blue;
    }
    if (key == QStringLiteral("office2021-blue") || key == QStringLiteral("office2021")) {
        return SARibbonTheme::RibbonThemeOffice2021Blue;
    }
    if (key == QStringLiteral("windows7") || key == QStringLiteral("win7")) {
        return SARibbonTheme::RibbonThemeWindows7;
    }
    return std::nullopt;
}

std::optional<SARibbonTheme> darkThemeForKey(const QString& key)
{
    if (key == QStringLiteral("dark")) {
        return SARibbonTheme::RibbonThemeDark;
    }
    if (key == QStringLiteral("dark2")) {
        return SARibbonTheme::RibbonThemeDark2;
    }
    return std::nullopt;
}

std::optional<SARibbonBar::RibbonStyles> ribbonStyleForKey(const QString& key)
{
    static const QHash<QString, SARibbonBar::RibbonStyles> styles = {
        {QStringLiteral("office"), SARibbonBar::RibbonStyleLooseThreeRow},
        {QStringLiteral("loose-three-row"), SARibbonBar::RibbonStyleLooseThreeRow},
        {QStringLiteral("compact-three-row"), SARibbonBar::RibbonStyleCompactThreeRow},
        {QStringLiteral("office-two-row"), SARibbonBar::RibbonStyleLooseTwoRow},
        {QStringLiteral("loose-two-row"), SARibbonBar::RibbonStyleLooseTwoRow},
        {QStringLiteral("wps"), SARibbonBar::RibbonStyleCompactTwoRow},
        {QStringLiteral("wps-lite"), SARibbonBar::RibbonStyleCompactTwoRow},
        {QStringLiteral("compact-two-row"), SARibbonBar::RibbonStyleCompactTwoRow},
        {QStringLiteral("office-single-row"), SARibbonBar::RibbonStyleLooseSingleRow},
        {QStringLiteral("loose-single-row"), SARibbonBar::RibbonStyleLooseSingleRow},
        {QStringLiteral("compact-single-row"), SARibbonBar::RibbonStyleCompactSingleRow},
    };
    const auto iterator = styles.constFind(key);
    return iterator == styles.constEnd() ? std::nullopt
                                         : std::optional<SARibbonBar::RibbonStyles>(iterator.value());
}

QString canonicalStyleKey(const QString& value)
{
    const auto style = ribbonStyleForKey(normalizeKey(value));
    if (!style)
        return {};
    if (*style == SARibbonBar::RibbonStyleLooseThreeRow)
        return QStringLiteral("office");
    if (*style == SARibbonBar::RibbonStyleCompactThreeRow)
        return QStringLiteral("compact-three-row");
    if (*style == SARibbonBar::RibbonStyleLooseTwoRow)
        return QStringLiteral("office-two-row");
    if (*style == SARibbonBar::RibbonStyleCompactTwoRow)
        return QStringLiteral("wps");
    if (*style == SARibbonBar::RibbonStyleLooseSingleRow)
        return QStringLiteral("office-single-row");
    if (*style == SARibbonBar::RibbonStyleCompactSingleRow)
        return QStringLiteral("compact-single-row");
    return {};
}

QString ribbonThemeName(SARibbonTheme theme)
{
    switch (theme) {
    case SARibbonTheme::RibbonThemeOffice2013:
        return QStringLiteral("office2013");
    case SARibbonTheme::RibbonThemeOffice2016Blue:
        return QStringLiteral("office2016-blue");
    case SARibbonTheme::RibbonThemeOffice2021Blue:
        return QStringLiteral("office2021-blue");
    case SARibbonTheme::RibbonThemeWindows7:
        return QStringLiteral("windows7");
    case SARibbonTheme::RibbonThemeDark:
        return QStringLiteral("dark");
    case SARibbonTheme::RibbonThemeDark2:
        return QStringLiteral("dark2");
    }
    return QStringLiteral("unknown");
}

QPalette lightPalette()
{
    QPalette palette;
    const QColor window(0xf3, 0xf3, 0xf3);
    const QColor text(0x1f, 0x1f, 0x1f);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
    palette.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x3b, 0x30));
    palette.setColor(QPalette::Link, QColor(0x1a, 0x66, 0xcc));
    palette.setColor(QPalette::Highlight, QColor(0x2a, 0x76, 0xc6));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    palette.setColor(QPalette::Light, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Midlight, QColor(0xe9, 0xe9, 0xe9));
    palette.setColor(QPalette::Mid, QColor(0xc4, 0xc4, 0xc4));
    palette.setColor(QPalette::Dark, QColor(0xa0, 0xa0, 0xa0));
    palette.setColor(QPalette::Shadow, QColor(0x6e, 0x6e, 0x6e));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0x9a, 0x9a));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0x9a, 0x9a));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0x9a, 0x9a));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0xd5, 0xd5, 0xd5));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0xf5, 0xf5, 0xf5));
    return palette;
}

QPalette darkPalette()
{
    QPalette palette;
    const QColor window(0x19, 0x24, 0x30);
    const QColor text(0xda, 0xda, 0xda);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, QColor(0x34, 0x42, 0x53));
    palette.setColor(QPalette::AlternateBase, QColor(0x2b, 0x37, 0x47));
    palette.setColor(QPalette::ToolTipBase, QColor(0x34, 0x42, 0x53));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(0x34, 0x42, 0x53));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    palette.setColor(QPalette::Link, QColor(0x6c, 0xb6, 0xff));
    palette.setColor(QPalette::Highlight, QColor(0x2a, 0x8d, 0xb5));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x96, 0xa3));
    palette.setColor(QPalette::Light, QColor(0x4d, 0x59, 0x73));
    palette.setColor(QPalette::Midlight, QColor(0x40, 0x50, 0x6a));
    palette.setColor(QPalette::Mid, QColor(0x24, 0x32, 0x41));
    palette.setColor(QPalette::Dark, QColor(0x0f, 0x16, 0x20));
    palette.setColor(QPalette::Shadow, QColor(0x08, 0x0d, 0x16));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x7a, 0x85, 0x91));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x7a, 0x85, 0x91));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x7a, 0x85, 0x91));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x2a, 0x35, 0x44));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0x8a, 0x94, 0xa0));
    return palette;
}

// SARibbon 内置深色 QSS 只覆盖 Ribbon 自身，停靠面板/场景树/菜单仍按浅色系统样式绘制。
// 这里补上这些区域的深色规则；窗口级样式表会在 setRibbonTheme 之后追加。
QString darkAuxiliaryStyleSheet()
{
    return QString::fromLatin1(R"QSS(
QToolTip {
    color: #dadada;
    background-color: #344253;
    border: 1px solid #4d5973;
    padding: 4px;
}
QMenu {
    color: #dadada;
    background-color: #2b3747;
    border: 1px solid #4d5973;
}
QMenu::item {
    padding: 6px 24px 6px 24px;
}
QMenu::item:selected {
    background-color: #2a8db5;
    color: #ffffff;
}
QMenu::separator {
    height: 1px;
    background: #4d5973;
    margin: 4px 8px;
}
QLineEdit, QComboBox, QAbstractSpinBox {
    color: #dadada;
    background-color: #344253;
    border: 1px solid #4d5973;
    padding: 3px;
    selection-background-color: #2a8db5;
    selection-color: #ffffff;
}
QTreeView, QListView, QTableView {
    color: #dadada;
    background-color: #344253;
    alternate-background-color: #2b3747;
    border: 1px solid #344253;
    selection-background-color: #2a8db5;
    selection-color: #ffffff;
}
QHeaderView::section {
    color: #dadada;
    background-color: #2b3747;
    border: 0;
    border-right: 1px solid #4d5973;
    padding: 4px;
}
QScrollBar:vertical {
    background: #192430;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #4d5973;
    min-height: 24px;
    border-radius: 6px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
QScrollBar:horizontal {
    background: #192430;
    height: 12px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #4d5973;
    min-width: 24px;
    border-radius: 6px;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0;
}
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: transparent;
}
ads--CDockManager, ads--CDockContainerWidget, ads--CDockAreaWidget,
ads--CFloatingDockContainer, ads--CAutoHideSideBar {
    background-color: #192430;
}
ads--CDockAreaTitleBar {
    background-color: #243241;
}
ads--CDockWidgetTab {
    color: #dadada;
}
)QSS");
}

class ThemeCommand final : public RF::PluginEngine::ICommand {
  public:
    ThemeCommand(ThemeController& controller, ThemeController::Mode mode, QString name,
                 QString caption, QString hint)
        : m_controller(controller), m_mode(mode), m_name(std::move(name)),
          m_caption(std::move(caption)), m_hint(std::move(hint)) {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("appearance"); }
    bool getChecked() const override { return m_controller.mode() == m_mode; }
    bool getEnabled() const override { return true; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_hint; }
    void onClick() override { m_controller.setMode(m_mode); }
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}

  private:
    ThemeController& m_controller;
    ThemeController::Mode m_mode;
    QString m_name;
    QString m_caption;
    QString m_hint;
};

class RibbonStyleCommand final : public RF::PluginEngine::ICommand {
  public:
    RibbonStyleCommand(ThemeController& controller, QString name, QString caption,
                       QString styleKey)
        : m_controller(controller), m_name(std::move(name)), m_caption(std::move(caption)),
          m_styleKey(std::move(styleKey)) {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("appearance"); }
    bool getChecked() const override { return m_controller.isStyleActive(m_styleKey); }
    bool getEnabled() const override { return true; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return QStringLiteral("切换 Ribbon 布局"); }
    void onClick() override { m_controller.setRibbonStyle(m_styleKey); }
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}

  private:
    ThemeController& m_controller;
    QString m_name;
    QString m_caption;
    QString m_styleKey;
};

class RibbonMinimizeCommand final : public RF::PluginEngine::ICommand {
  public:
    QString getCaption() const override { return QStringLiteral("Ribbon"); }
    QString getCategory() const override { return QStringLiteral("window"); }
    bool getChecked() const override
    {
        const SARibbonBar* bar = ribbon();
        return bar && bar->isMinimumMode();
    }
    bool getEnabled() const override { return ribbon() != nullptr; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.ribbon.minimize"); }
    QString getTooltip() const override { return QStringLiteral("收起或展开 Ribbon"); }
    void onClick() override
    {
        if (auto* bar = ribbon())
            bar->setMinimumMode(!bar->isMinimumMode());
    }
    void onCreate(RF::PluginEngine::IApplication* application) override
    {
        m_application = application;
    }
    void onDestroy() override { m_application = nullptr; }

  private:
    SARibbonBar* ribbon() const
    {
        auto* window = m_application
            ? qobject_cast<SARibbonMainWindow*>(m_application->getMainWindow())
            : nullptr;
        return window ? window->ribbonBar() : nullptr;
    }

    RF::PluginEngine::IApplication* m_application = nullptr;
};

class ThemeChoiceCommand final : public RF::PluginEngine::IChoiceCommand {
  public:
    explicit ThemeChoiceCommand(ThemeController& controller) : m_controller(controller) {}

    QString getCaption() const override { return QStringLiteral("主题"); }
    QString getCategory() const override { return QStringLiteral("appearance"); }
    bool getChecked() const override { return true; }
    bool getEnabled() const override { return true; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return QStringLiteral("cmd.theme.variant"); }
    QString getTooltip() const override { return QStringLiteral("选择浅色或深色主题"); }
    QString currentValue() const override { return m_controller.ribbonThemeKey(); }
    QString currentOptionGroup() const override { return m_controller.modeKey(); }
    bool selectValue(const QString& value) override
    {
        if (lightThemeForKey(normalizeKey(value)))
            return m_controller.setLightTheme(value);
        if (darkThemeForKey(normalizeKey(value)))
            return m_controller.setDarkTheme(value);
        return false;
    }
    void onClick() override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}

  private:
    ThemeController& m_controller;
};

class WindowPanelCommand final : public RF::PluginEngine::ICommand {
  public:
    WindowPanelCommand(QString name, QString caption, QString hint)
        : m_name(std::move(name)), m_caption(std::move(caption)), m_hint(std::move(hint)) {
    }

    QString getCaption() const override { return m_caption; }
    QString getCategory() const override { return QStringLiteral("window"); }
    bool getChecked() const override { return false; }
    bool getEnabled() const override { return true; }
    QString getMessage() const override { return {}; }
    QString getName() const override { return m_name; }
    QString getTooltip() const override { return m_hint; }
    void onClick() override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}

  private:
    QString m_name;
    QString m_caption;
    QString m_hint;
};

struct RibbonStyleCommandSpec {
    const char* command;
    const char* styleKey;
    const char* caption;
};

const RibbonStyleCommandSpec ribbonStyleCommands[] = {
    {"cmd.ribbon.style.loose_three", "office", "宽松三行"},
    {"cmd.ribbon.style.compact_three", "compact-three-row", "紧凑三行"},
    {"cmd.ribbon.style.loose_two", "office-two-row", "宽松两行"},
    {"cmd.ribbon.style.compact_two", "wps", "紧凑两行"},
    {"cmd.ribbon.style.loose_single", "office-single-row", "宽松单行"},
    {"cmd.ribbon.style.compact_single", "compact-single-row", "紧凑单行"},
};

RF::PluginEngine::IPlugin* createHostCommand(
    const RF::PluginEngine::PluginCreationContext& context, ThemeController& controller)
{
    const QString name = context.attributes.value(QStringLiteral("name"));
    if (name == QStringLiteral("cmd.theme.light")) {
        return new ThemeCommand(controller, ThemeController::Mode::Light, name,
                                QStringLiteral("浅色主题"), QStringLiteral("切换为浅色界面"));
    }
    if (name == QStringLiteral("cmd.theme.dark")) {
        return new ThemeCommand(controller, ThemeController::Mode::Dark, name,
                                QStringLiteral("深色主题"), QStringLiteral("切换为深色界面"));
    }
    if (name == QStringLiteral("cmd.theme.variant"))
        return new ThemeChoiceCommand(controller);
    if (name == QStringLiteral("cmd.window.plugins")) {
        return new WindowPanelCommand(name, QStringLiteral("插件管理"),
                                      QStringLiteral("显示或隐藏插件管理"));
    }
    if (name == QStringLiteral("cmd.window.log")) {
        return new WindowPanelCommand(name, QStringLiteral("日志"),
                                      QStringLiteral("显示或隐藏运行日志"));
    }
    if (name == QStringLiteral("cmd.ribbon.minimize"))
        return new RibbonMinimizeCommand();
    for (const RibbonStyleCommandSpec& spec : ribbonStyleCommands) {
        if (name == QString::fromLatin1(spec.command)) {
            return new RibbonStyleCommand(controller, name, QString::fromUtf8(spec.caption),
                                          QString::fromLatin1(spec.styleKey));
        }
    }
    return nullptr;
}

} // namespace

QString hostCommandPluginId()
{
    return QStringLiteral("plugin.view3d.host");
}

ThemeController::ThemeController(SARibbonMainWindow& window, QString settingsPath, bool persist)
    : m_window(window), m_settingsPath(std::move(settingsPath)), m_persist(persist)
{
}

void ThemeController::initialize(const QString& style, const QString& theme,
                                 QStringList* warnings)
{
    m_styleKey = canonicalStyleKey(style);
    if (m_styleKey.isEmpty()) {
        if (!style.trimmed().isEmpty() && warnings) {
            warnings->append(QStringLiteral("未知的 Ribbon 风格：%1，回退为 office")
                                 .arg(style));
        }
        m_styleKey = QStringLiteral("office");
    }

    const QString themeKey = normalizeKey(theme);
    if (themeKey.isEmpty()) {
        m_mode = Mode::Light;
    } else if (const auto light = lightThemeForKey(themeKey)) {
        m_lightTheme = *light;
        m_mode = Mode::Light;
    } else if (const auto dark = darkThemeForKey(themeKey)) {
        m_darkTheme = *dark;
        m_mode = Mode::Dark;
    } else {
        if (warnings) {
            warnings->append(QStringLiteral("未知的界面主题：%1，回退为 light")
                                 .arg(theme));
        }
        m_mode = Mode::Light;
    }

    if (m_persist) {
        QSettings settings(m_settingsPath, QSettings::IniFormat);
        const QString persisted =
            normalizeKey(settings.value(QStringLiteral("ui/theme")).toString());
        if (persisted == QStringLiteral("light") || persisted == QStringLiteral("dark"))
            m_mode = persisted == QStringLiteral("dark") ? Mode::Dark : Mode::Light;
        const QString persistedStyle =
            canonicalStyleKey(settings.value(QStringLiteral("ui/ribbonStyle")).toString());
        if (!persistedStyle.isEmpty())
            m_styleKey = persistedStyle;
        const QString persistedLight = normalizeKey(
            settings.value(QStringLiteral("ui/lightTheme")).toString());
        if (const auto light = lightThemeForKey(persistedLight))
            m_lightTheme = *light;
        const QString persistedDark = normalizeKey(
            settings.value(QStringLiteral("ui/darkTheme")).toString());
        if (const auto dark = darkThemeForKey(persistedDark))
            m_darkTheme = *dark;
    }
}

void ThemeController::applyStyle(SARibbonBar* ribbon)
{
    m_ribbon = ribbon;
    if (!m_ribbon)
        return;
    const auto style = ribbonStyleForKey(m_styleKey);
    m_ribbon->setRibbonStyle(style.value_or(SARibbonBar::RibbonStyleLooseThreeRow));
}

void ThemeController::applyTheme()
{
    qApp->setPalette(paletteFor(m_mode));
    m_window.setRibbonTheme(currentRibbonTheme());
    if (isDark()) {
        QString sheet = m_window.styleSheet();
        sheet += darkAuxiliaryStyleSheet();
        m_window.setStyleSheet(sheet);
    }
}

void ThemeController::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    applyTheme();
    persistSettings();
}

bool ThemeController::setLightTheme(const QString& key)
{
    const auto light = lightThemeForKey(normalizeKey(key));
    if (!light)
        return false;
    m_lightTheme = *light;
    m_mode = Mode::Light;
    applyTheme();
    persistSettings();
    return true;
}

bool ThemeController::setDarkTheme(const QString& key)
{
    const auto dark = darkThemeForKey(normalizeKey(key));
    if (!dark)
        return false;
    m_darkTheme = *dark;
    m_mode = Mode::Dark;
    applyTheme();
    persistSettings();
    return true;
}

void ThemeController::setRibbonStyle(const QString& style)
{
    const QString canonical = canonicalStyleKey(style);
    if (canonical.isEmpty())
        return;
    m_styleKey = canonical;
    const auto resolved = ribbonStyleForKey(m_styleKey);
    if (m_ribbon && resolved && m_ribbon->currentRibbonStyle() != *resolved)
        m_ribbon->setRibbonStyle(*resolved);
    persistSettings();
}

bool ThemeController::isStyleActive(const QString& style) const
{
    const QString canonical = canonicalStyleKey(style);
    return !canonical.isEmpty() && canonical == m_styleKey;
}

QString ThemeController::modeKey() const
{
    return isDark() ? QStringLiteral("dark") : QStringLiteral("light");
}

QString ThemeController::ribbonThemeKey() const
{
    return View3DApplication::ribbonThemeName(currentRibbonTheme());
}

QString ThemeController::lightThemeKey() const
{
    return View3DApplication::ribbonThemeName(m_lightTheme);
}

QString ThemeController::darkThemeKey() const
{
    return View3DApplication::ribbonThemeName(m_darkTheme);
}

SARibbonTheme ThemeController::currentRibbonTheme() const
{
    return isDark() ? m_darkTheme : m_lightTheme;
}

bool ThemeController::ribbonStyleMatches(const SARibbonBar* ribbon) const
{
    if (!ribbon)
        return false;
    const auto style = ribbonStyleForKey(m_styleKey);
    return style && ribbon->currentRibbonStyle() == *style;
}

QPalette ThemeController::paletteFor(Mode mode)
{
    return mode == Mode::Dark ? darkPalette() : lightPalette();
}

void ThemeController::persistSettings() const
{
    if (!m_persist)
        return;
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/theme"), modeKey());
    settings.setValue(QStringLiteral("ui/lightTheme"), lightThemeKey());
    settings.setValue(QStringLiteral("ui/darkTheme"), darkThemeKey());
    settings.setValue(QStringLiteral("ui/ribbonStyle"), m_styleKey);
    settings.sync();
}

void registerThemeCommandFactory(RF::PluginEngine::PluginFactoryRegistry& registry,
                                 ThemeController& controller)
{
    for (const QString& type :
         {QStringLiteral("command"), QStringLiteral("checkcommand"),
          QStringLiteral("dropdown")}) {
        registry.registerCreator(
            hostCommandPluginId(), QStringLiteral("factory.create_command"), type,
            [&controller](const RF::PluginEngine::PluginCreationContext& context) {
                return createHostCommand(context, controller);
            });
    }
}

} // namespace View3DApplication
