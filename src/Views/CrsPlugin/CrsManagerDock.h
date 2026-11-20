#pragma once

#include <ICrsBrowser.h>
#include <ICrsLibrary.h>
#include <IDockWindow.h>

#include <QString>

class QAction;
class QLabel;
class QPushButton;
class QWidget;

namespace View3DCrs {
class CrsBrowserWidget;
}

namespace RF::PluginEngine {
class IApplication;
class IPluginContainer;
} // namespace RF::PluginEngine

namespace View3D {
class IGeoReference;
} // namespace View3D

namespace View3DCrs {

// 参照系管理面板：工具栏（新建/导入/导出）+ 共用的浏览控件 + 状态行。
// 内置只读；自定义条目通过「新建…」对话框增改、右键菜单删除；所有会改库的操作
// （新建/编辑/删除/收藏/导入）都立刻写回库文件。双击列表项 = 设为显示参照系。
class CrsManagerDock final : public RF::PluginEngine::IDockWindow {
  public:
    CrsManagerDock(QString name, QString caption);

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Right;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool isActive) override;
    void onCreate(RF::PluginEngine::IApplication* app) override;
    void onDestroy() override;
    QWidget* getWidget() const override { return m_widget; }
    // 三栏各自滚动，面板本身不能被停靠框架的滚动区按最小尺寸托住。
    bool getScrollable() const override { return false; }

    // 重新取服务并刷新；扩展发布完库后也会主动调一次。
    void refreshBindings();

  private:
    void buildUi();
    void buildBrowser();
    void updateActions();
    void showContextMenu();
    void startNewEntry();
    void editCurrent();
    void promoteToCustom();
    void removeCurrent();
    void toggleFavorite();
    void applyAsDisplay();
    void importLibrary();
    void exportLibrary();
    bool persist();
    void setStatus(const QString& text, bool failed = false);

    QString m_name;
    QString m_caption;
    QWidget* m_widget = nullptr;
    CrsBrowserWidget* m_browserWidget = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_newButton = nullptr;
    QPushButton* m_importButton = nullptr;
    QPushButton* m_exportButton = nullptr;
    QAction* m_favoriteAction = nullptr;
    QAction* m_editAction = nullptr;
    QAction* m_promoteAction = nullptr;
    QAction* m_deleteAction = nullptr;

    RF::PluginEngine::IApplication* m_application = nullptr;
    RF::PluginEngine::IPluginContainer* m_container = nullptr;
    View3D::ICrsLibrary* m_library = nullptr;
    View3D::IGeoReference* m_reference = nullptr;
    View3D::ICrsBrowser* m_browser = nullptr;
    quint64 m_subscription = 0;
};

} // namespace View3DCrs
