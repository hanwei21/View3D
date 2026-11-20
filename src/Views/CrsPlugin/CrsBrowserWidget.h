#pragma once

#include <ICrsBrowser.h>
#include <ICrsLibrary.h>

#include <QPoint>
#include <QString>
#include <QWidget>

#include <functional>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace View3DCrs {

// 参照系浏览控件：搜索 + 类型过滤 + 左树（常用 / 自定义 / EPSG 内置分组）+ 列表
// + 详情卡片（含「复制 proj4 / 复制 WKT2」）。参照系管理面板与「选择参照系」
// 对话框共用这一份实现，两处的搜索与详情行为不会分叉。
// 它只负责浏览与选中；改库动作（收藏/编辑/删除/新建）由宿主通过回调决定。
class CrsBrowserWidget final : public QWidget {
  public:
    CrsBrowserWidget(View3D::ICrsLibrary* library, View3D::ICrsBrowser* browser,
                     QWidget* parent = nullptr);

    // 库或收藏变了就调它：重建左树与列表，尽量保持原来的选中项。
    void refresh();
    void selectCustomBranch();
    // 在当前列表里选中某个定义；找不到就清掉选中（选择器的「确定」会因此变灰）。
    void selectDefinition(const QString& definition);

    QString currentDefinition() const;
    QString currentLibraryId() const;
    bool currentIsCustom() const;
    // 非自定义的选中行（内置或收藏回指）：删除/编辑不能对它用。
    bool currentIsBuiltin() const;
    bool hasSelection() const;

    void setOnSelectionChanged(std::function<void()> callback);
    void setOnContextMenuRequested(std::function<void(const QPoint&)> callback);
    void setOnActivated(std::function<void()> callback);   // 双击列表项
    QPoint lastContextPosition() const { return m_contextPos; }
    // 列表为空 / 结果过多 / 没搜到这类的提示，宿主把它显示到自己的状态栏。
    QString hint() const { return m_hint; }

  private:
    enum class Source { Favourites, Custom, Builtin };

    void rebuildTree();
    void populateList();
    void showDetails();
    Source currentSource() const;

    View3D::ICrsLibrary* m_library = nullptr;
    View3D::ICrsBrowser* m_browser = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_kindFilter = nullptr;
    QTreeWidget* m_tree = nullptr;
    QListWidget* m_list = nullptr;
    QTextBrowser* m_details = nullptr;
    QPushButton* m_copyProjButton = nullptr;
    QPushButton* m_copyWktButton = nullptr;
    QString m_builtinGroupId;
    QString m_detailProj;
    QString m_detailWkt;
    QString m_hint;   // 列表为空 / 结果过多等提示，由宿主显示到自己的状态栏
    QPoint m_contextPos;
    std::function<void()> m_onSelectionChanged;
    std::function<void(const QPoint&)> m_onContextMenu;
    std::function<void()> m_onActivated;
};

} // namespace View3DCrs
