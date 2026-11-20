#pragma once

#include <IDockWindow.h>

#include <QList>
#include <QString>

class QLabel;
class QPoint;
class QTableWidget;
class QWidget;

namespace View3DPlugins {

class MeasureSession;

class MeasureDock final : public RF::PluginEngine::IDockWindow {
  public:
    MeasureDock(QString name, QString caption);

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Bottom;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override { m_session = nullptr; }
    QWidget* getWidget() const override { return m_widget; }

    void setSession(MeasureSession* session);
    void setHint(const QString& text);
    void setRows(const QList<QStringList>& rows, int displayedRow);

  private:
    void showContextMenu(const QPoint& position);

    QString m_name;
    QString m_caption;
    QWidget* m_widget = nullptr;
    QLabel* m_hint = nullptr;
    QTableWidget* m_table = nullptr;
    MeasureSession* m_session = nullptr;
    bool m_updating = false;
};

} // namespace View3DPlugins
