#pragma once

#include "ICommand.h"

#include <QList>
#include <QString>

namespace RF {
namespace PluginEngine {

// 下拉的一项：value 是回传给命令的标识，caption 是显示文本，
// group 配合 currentOptionGroup() 做分组过滤。
struct ChoiceOption {
    QString value;
    QString caption;
    QString group;
};

// 配置驱动的下拉控件命令：选项由 ui_config 的 option 子节点声明，
// 选中值交给命令处理。
class IChoiceCommand : public ICommand {
public:
    virtual ~IChoiceCommand() = default;

    virtual QString currentValue() const = 0;
    virtual bool selectValue(const QString& value) = 0;
    // 非空时下拉只显示 option 中 group 相同的项（如浅色/深色主题分组）。
    virtual QString currentOptionGroup() const { return {}; }
    // 运行期动态选项：参照系列表新建一条，下拉就该多一项。宿主把它与 XML 静态项
    // 合并，同一个 value 以这里的为准；返回空表示完全用 XML 的列表。
    virtual QList<ChoiceOption> choiceOptions() const { return {}; }
};

} // namespace PluginEngine
} // namespace RF
