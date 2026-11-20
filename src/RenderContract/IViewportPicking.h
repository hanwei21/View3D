#pragma once

#include "ViewportPickResult.h"

#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

namespace View3D {

// 拾取监听者标识。视口可能有多个消费者（测量的表面拾取、要素属性表的高亮），
// 因此拾取按监听者列表广播，不再占用单个回调槽位互相覆盖。
using PickListenerId = std::uint64_t;

class IViewportPicking {
  public:
    virtual ~IViewportPicking() = default;

    // 返回的 id 用于注销；0 保留给「未注册」。
    virtual PickListenerId addPickListener(
        std::function<void(const ViewportPickResult&)> listener) = 0;
    virtual bool removePickListener(PickListenerId id) = 0;
    // 悬停拾取按独立的监听者列表广播：视口底部的坐标读数要用，它既不该跟着点击
    // 监听者一起被覆盖，也不该把没有命中模型的情况吞掉——这时给的是视线与地面
    // (y=0) 的交点；连地面都碰不到时 hasWorldPosition 为 false。
    virtual PickListenerId addHoverPickListener(
        std::function<void(const ViewportPickResult&)> listener) = 0;
    virtual bool removeHoverPickListener(PickListenerId id) = 0;
    virtual void setRectSelectCallback(
        std::function<void(const QList<QString>&, bool additive)> callback) = 0;
};

} // namespace View3D
