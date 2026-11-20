#pragma once

#include <ICrsBrowser.h>

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace View3DCrs {

// 新建 / 编辑自定义参照系的模态对话框：名称 + 定义 + 备注，定义一改就用 PROJ
// 解析一遍并给出预览（官方名、类型、投影方法），认不出来的定义不允许确定。
// 「确定」只在定义可解析且非空时可用，所以入库路径不会出现半截数据。
class CrsEditorDialog final : public QDialog {
  public:
    CrsEditorDialog(const View3D::ICrsBrowser* browser, QWidget* parent);

    // 编辑已有条目时把原值带进来；新建时留空即可。
    void setEntry(const QString& name, const QString& definition, const QString& note);

    QString entryName() const;
    QString definition() const;
    QString note() const;

  private:
    void refreshPreview();
    bool validate(QString* reason) const;

    const View3D::ICrsBrowser* m_browser = nullptr;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_definition = nullptr;
    QLineEdit* m_note = nullptr;
    QLabel* m_preview = nullptr;
    QPushButton* m_ok = nullptr;
};

} // namespace View3DCrs
