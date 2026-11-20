#pragma once

#include <QString>
#include <QStringList>

namespace View3DOsb {

// osgDB 找读取器插件的两个候选根目录：<exe目录>\plugins 与 <exe目录>。
// 两者都相对 **exe 所在目录**（我们部署时 exe 在 bin 下，所以才像是"从 bin 找"）；
// 真正的插件目录是 <root>\osgPlugins-<osg 版本>。
QStringList osgPluginRoots();

// 实际存在的 osgPlugins-* 目录，按 roots 顺序。
QStringList osgPluginDirs();

// 给错误提示用：把实际查找过的绝对路径串起来，换机器时一眼能看出该把目录放哪。
QString osgPluginSearchHint();

} // namespace View3DOsb
