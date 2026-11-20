#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

class QObject;

namespace Ogre {
class Root;
}

namespace View3D {

// 进程级 Ogre 运行时：Root、HLMS、串行化闸门与跨线程投递。
// 所有 Ogre 状态（Root、场景管理器、纹理、视口）只能在持有 gate() 时访问；
// 帧循环运行在渲染线程，UI 线程的调用同样经过该闸门。
class OgreRuntime {
  public:
    OgreRuntime();
    ~OgreRuntime();
    OgreRuntime(const OgreRuntime&) = delete;
    OgreRuntime& operator=(const OgreRuntime&) = delete;

    // initialize/shutdown 由渲染线程执行；UI 线程只等待握手结果。
    bool initialize();
    bool ensureHlms(const QString& runtimeDirectory);
    void shutdown();

    bool ready() const noexcept;
    bool hlmsReady() const noexcept;
    Ogre::Root* root() const noexcept;

    std::recursive_mutex& gate() noexcept { return m_gate; }
    // 默认关闭垂直同步：实测它不改善节拍，反而拉长整帧持锁时间。
    // VIEW3D_VSYNC=1（或 yes/on）可临时打开做对比。
    static bool vsyncEnabled();
    // 把回调投递回 UI 线程执行；渲染线程禁止直接操作 Qt 控件。
    void setUiContext(QObject* context) noexcept { m_uiContext = context; }
    void postToUi(std::function<void()> callback) const;

  private:
    struct State;
    std::unique_ptr<State> m_state;
    mutable std::recursive_mutex m_gate;
    QObject* m_uiContext = nullptr;
};

std::uint64_t nextOgreResourceSerial();

} // namespace View3D
