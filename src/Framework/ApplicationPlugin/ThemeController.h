#pragma once

#include <QPalette>
#include <QString>
#include <QStringList>

#include <SARibbonGlobal.h>

class SARibbonBar;
class SARibbonMainWindow;

namespace RF::PluginEngine {
class PluginFactoryRegistry;
}

namespace View3DApplication {

QString hostCommandPluginId();

// 负责把 ui_config 的 style/theme 落到 SARibbon，并维护一套配套的
// QPalette + 深色补充 QSS，使停靠面板、场景树等非 Ribbon 区域跟随主题。
class ThemeController final {
  public:
    enum class Mode { Light, Dark };

    ThemeController(SARibbonMainWindow& window, QString settingsPath, bool persist);

    void initialize(const QString& style, const QString& theme, QStringList* warnings);
    void applyStyle(SARibbonBar* ribbon);
    void applyTheme();

    void setMode(Mode mode);
    bool setLightTheme(const QString& key);
    bool setDarkTheme(const QString& key);
    void setRibbonStyle(const QString& style);
    bool isStyleActive(const QString& style) const;
    Mode mode() const { return m_mode; }
    bool isDark() const { return m_mode == Mode::Dark; }
    QString modeKey() const;
    QString lightThemeKey() const;
    QString darkThemeKey() const;
    QString styleKey() const { return m_styleKey; }
    QString ribbonThemeKey() const;
    SARibbonTheme currentRibbonTheme() const;
    bool ribbonStyleMatches(const SARibbonBar* ribbon) const;

    static QPalette paletteFor(Mode mode);

  private:
    void persistSettings() const;

    SARibbonMainWindow& m_window;
    QString m_settingsPath;
    bool m_persist = true;
    QString m_styleKey = QStringLiteral("office");
    Mode m_mode = Mode::Light;
    SARibbonTheme m_lightTheme = SARibbonTheme::RibbonThemeOffice2013;
    SARibbonTheme m_darkTheme = SARibbonTheme::RibbonThemeDark2;
    SARibbonBar* m_ribbon = nullptr;
};

void registerThemeCommandFactory(RF::PluginEngine::PluginFactoryRegistry& registry,
                                 ThemeController& controller);

} // namespace View3DApplication
