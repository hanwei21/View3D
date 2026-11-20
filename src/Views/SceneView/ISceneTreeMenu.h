#pragma once

#include "CoreTypes.h"

#include <QList>
#include <QString>

#include <functional>

namespace View3D {

enum class SceneTreeMenuTargetKind {
    SceneRoot,
    Group,
    Layer
};

struct SceneTreeMenuTarget {
    SceneTreeMenuTargetKind kind = SceneTreeMenuTargetKind::SceneRoot;
    // 右键命中的组或图层；场景根为空。
    LayerId primary;
    // 当前选中的全部图层（多选时包含 primary）。场景根为空。
    QList<LayerId> layers;
};

struct SceneTreeMenuAction {
    QString text;
    // resources/icons 下的图标名（不含路径与扩展名）；空表示不显示图标。
    QString icon;
    bool enabled = true;
    bool separatorBefore = false;
    std::function<void()> triggered;
};

// 数据插件向场景树右键菜单注入自己的条目。目录树只认识图层 id 与类型，
// 具体图层的定位、重载、卸载等操作由持有该图层的插件提供。
class ISceneTreeMenuContributor {
  public:
    virtual ~ISceneTreeMenuContributor() = default;

    virtual void appendSceneTreeMenu(const SceneTreeMenuTarget& target,
                                     QList<SceneTreeMenuAction>* actions) = 0;
    // 图层内容归本插件所有时返回 true；目录树删除/清空这类图层会交给
    // removeOwnedSceneTreeLayer，避免只删了树上的条目而内容还在后台运行。
    virtual bool ownsSceneTreeLayer(const LayerId& layerId) const = 0;
    virtual bool removeOwnedSceneTreeLayer(const LayerId& layerId) = 0;
};

class ISceneTreeMenuRegistry {
  public:
    virtual ~ISceneTreeMenuRegistry() = default;

    virtual void addSceneTreeMenuContributor(ISceneTreeMenuContributor* contributor) = 0;
    virtual void removeSceneTreeMenuContributor(ISceneTreeMenuContributor* contributor) = 0;
    virtual QList<ISceneTreeMenuContributor*> sceneTreeMenuContributors() const = 0;
};

} // namespace View3D
