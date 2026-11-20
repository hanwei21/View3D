#pragma once

#include <QList>
#include <QPoint>
#include <QString>

#include <functional>

namespace View3D {

class IRenderSurface;

struct ViewportMenuContext {
    // 被右键的视口；位置是视口控件内坐标。
    IRenderSurface* view = nullptr;
    QPoint position;
};

struct ViewportMenuAction {
    QString text;
    // resources/icons 下的图标名（不含路径与扩展名）；空表示不显示图标。
    QString icon;
    bool enabled = true;
    bool separatorBefore = false;
    std::function<void()> triggered;
};

// 工具/数据插件向三维视口右键菜单注入条目。SceneView 只负责判定“右键点击”
// 与弹出菜单，不认识具体工具；没有贡献者返回条目时右键保持原有行为。
class IViewportMenuContributor {
  public:
    virtual ~IViewportMenuContributor() = default;

    virtual void appendViewportMenu(const ViewportMenuContext& context,
                                    QList<ViewportMenuAction>* actions) = 0;
};

class IViewportMenuRegistry {
  public:
    virtual ~IViewportMenuRegistry() = default;

    virtual void addViewportMenuContributor(IViewportMenuContributor* contributor) = 0;
    virtual void removeViewportMenuContributor(IViewportMenuContributor* contributor) = 0;
    virtual QList<IViewportMenuContributor*> viewportMenuContributors() const = 0;
};

} // namespace View3D
