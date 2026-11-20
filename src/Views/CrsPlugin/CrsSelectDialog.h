#pragma once

#include <QDialog>
#include <QString>

class QPushButton;

namespace View3D {
class ICrsBrowser;
class ICrsLibrary;
} // namespace View3D

namespace View3DCrs {

class CrsBrowserWidget;

// 「选择显示参照系」对话框：和参照系面板共用同一套浏览控件，内置/常用/自定义
// 都能挑；确定后由调用方拿去 setDisplayCrs。没有选中项时「确定」是灰的。
class CrsSelectDialog final : public QDialog {
  public:
    CrsSelectDialog(View3D::ICrsLibrary* library, View3D::ICrsBrowser* browser,
                    QWidget* parent);
    // 打开时预选当前生效的那条（找不到就保持不选，「确定」是灰的）。
    void preselect(const QString& definition);
    QString selectedDefinition() const;

  private:
    CrsBrowserWidget* m_browser = nullptr;
    QPushButton* m_ok = nullptr;
};

} // namespace View3DCrs
