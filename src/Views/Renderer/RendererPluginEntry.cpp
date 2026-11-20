#include "View3DPluginEntry.h"

#include "OgreRuntime.h"

#include <OgreRoot.h>
#include "OgreScene.h"
#include "OgreViewportSurface.h"

#include <IApplication.h>
#include <IExtension.h>
#include <IPlugin.h>
#include <IPluginContainer.h>
#include <IRenderScene.h>
#include <IRenderService.h>
#include <View3DObjectIds.h>

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace RF::PluginEngine;

// 帧循环运行在独立渲染线程；Qt 控件与图层模型留在 UI 线程。
// 两侧共享的 Ogre 状态由 OgreRuntime::gate() 串行化，
// 生命周期与预处理结果通过命令队列投递到渲染线程。
class OgreRenderService final : public IExtension, public View3D::IRenderService {
  public:
    bool onCreate(IApplication* application) override
    {
        auto* container = application ? application->getPluginContainer() : nullptr;
        if (!container)
            return false;
        m_application = application;
        m_runtime.setUiContext(QCoreApplication::instance());
        std::promise<bool> startup;
        std::future<bool> startupResult = startup.get_future();
        m_renderThread = std::thread([this, &startup] { renderLoop(startup); });
        const bool runtimeReady = startupResult.get();
        std::cerr << "VIEW3D_RENDER_THREAD ui=" << std::hash<std::thread::id>()(std::this_thread::get_id())
                  << " render=" << std::hash<std::thread::id>()(m_renderThread.get_id())
                  << " runtime_ready=" << (runtimeReady ? 1 : 0) << std::endl;

        View3D::OgreScene* defaultScene = nullptr;
        if (runtimeReady) {
            defaultScene = dynamic_cast<View3D::OgreScene*>(createScene());
            m_scene = defaultScene;
        }
        if (!runtimeReady || !m_scene) {
            std::cerr << "VIEW3D_RENDER_SERVICE_FAILED runtime_ready="
                      << (runtimeReady ? 1 : 0) << " scene=" << (m_scene ? 1 : 0)
                      << std::endl;
            joinRenderThread();
            m_application = nullptr;
            return false;
        }

        container->publishAs<View3D::IRenderService>(View3D::ServiceIds::Renderer, this, this);
        container->publishAs<View3D::IRenderScene>(View3D::ServiceIds::Scene, m_scene, this);
        m_rendering.store(true, std::memory_order_release);
        return true;
    }

    void onDestroy() override
    {
        // 先在渲染线程上停掉帧循环，保证下面的销毁过程没有并发遍历。
        callSync<void>([this] { m_rendering.store(false, std::memory_order_relaxed); });
        if (m_application) {
            if (auto* container = m_application->getPluginContainer()) {
                container->unpublishObject(View3D::ServiceIds::Scene, m_scene);
                container->unpublishObject(View3D::ServiceIds::Renderer,
                                           static_cast<View3D::IRenderService*>(this));            }
        }
        {
            std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
            for (View3D::OgreViewportSurface* view : std::as_const(m_views))
                delete view;
            m_views.clear();
            for (View3D::OgreScene* scene : std::as_const(m_scenes))
                delete scene;
            m_scenes.clear();
            m_scene = nullptr;
        }
        // Root 由渲染线程创建，退出路径同样在该线程上销毁。
        joinRenderThread();
        printPacingStats("shutdown");
        m_application = nullptr;
    }

    View3D::IRenderScene* createScene() override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        auto* scene = new View3D::OgreScene(m_runtime);
        m_scenes.append(scene);
        return scene;
    }

    void destroyScene(View3D::IRenderScene* scene) override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        if (!scene || scene == m_scene)
            return;
        for (View3D::OgreViewportSurface* view : std::as_const(m_views)) {
            if (view->scene() == scene)
                return;
        }
        auto* ogreScene = dynamic_cast<View3D::OgreScene*>(scene);
        if (!ogreScene)
            return;
        m_scenes.removeAll(ogreScene);
        delete ogreScene;
    }

    View3D::IRenderSurface* createView(View3D::IRenderScene* scene,
                                       QWidget* parent) override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        auto* ogreScene = dynamic_cast<View3D::OgreScene*>(scene);
        if (!ogreScene)
            return nullptr;
        auto* view = new View3D::OgreViewportSurface(m_runtime, *ogreScene, parent);
        if (!view->initialize(QCoreApplication::applicationDirPath())) {
            delete view;
            return nullptr;
        }
        m_views.append(view);
        return view;
    }

    void destroyView(View3D::IRenderSurface* view) override
    {
        std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
        auto* ogreView = dynamic_cast<View3D::OgreViewportSurface*>(view);
        if (!ogreView)
            return;
        m_views.removeAll(ogreView);
        delete ogreView;
    }

    void setTargetFrameRate(double framesPerSecond) override
    {
        const double clamped = qBound(1.0, framesPerSecond, 240.0);
        m_targetFrameRate.store(clamped, std::memory_order_relaxed);
        m_frameIntervalMs.store(qMax(1, qRound(1000.0 / clamped)), std::memory_order_relaxed);
    }

    double targetFrameRate() const noexcept override
    {
        return m_targetFrameRate.load(std::memory_order_relaxed);
    }

    std::uint64_t frameCount() const noexcept override
    {
        return m_frameCount.load(std::memory_order_relaxed);
    }

    // 渲染线程专属任务队列：UI 线程投递、渲染线程按帧顺序执行。
    void postCommand(std::function<void()> command)
    {
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commands.push_back(std::move(command));
        }
        m_commandCondition.notify_one();
    }

    // 在渲染线程执行并等待结果；线程已退出或已在渲染线程时就地执行。
    template <typename Result>
    Result callSync(std::function<Result()> work)
    {
        if (m_threadFinished.load(std::memory_order_acquire) ||
            std::this_thread::get_id() == m_renderThreadId)
            return work();
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(work));
        std::future<Result> result = task->get_future();
        postCommand([task] { (*task)(); });
        return result.get();
    }

  private:
    void renderLoop(std::promise<bool>& startup)
    {
        m_renderThreadId = std::this_thread::get_id();
#ifdef Q_OS_WIN
        // Windows 默认 15.6ms 定时器粒度会把 16ms 的帧等待顶成 31ms。
        timeBeginPeriod(1u);
#endif
        bool ready = false;
        try {
            ready = m_runtime.initialize();
        } catch (const std::exception& error) {
            std::cerr << "VIEW3D_RENDER_THREAD_FAILED: " << error.what() << std::endl;
            ready = false;
        }
        startup.set_value(ready);
        if (ready) {
            while (!m_stopRequested.load(std::memory_order_acquire)) {
                drainCommands();
                renderCycle();
                waitNextFrame();
            }
            drainCommands();
            shutdownRuntime();
        }
#ifdef Q_OS_WIN
        timeEndPeriod(1u);
#endif
        m_threadFinished.store(true, std::memory_order_release);
    }

    void renderCycle()
    {
        if (!m_rendering.load(std::memory_order_relaxed))
            return;
        const auto cycleStart = std::chrono::steady_clock::now();
        std::size_t activeCount = 0;
        {
            std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
            for (View3D::OgreScene* scene : std::as_const(m_scenes)) {
                scene->processPendingUploads();
                scene->updateDistanceVisibility();
            }
            bool anyActive = false;
            for (View3D::OgreViewportSurface* view : std::as_const(m_views)) {
                if (view->renderActive()) {
                    view->prepareFrame();
                    anyActive = true;
                    ++activeCount;
                }
            }
            // 球面模式：锚点由最后一个 prepareFrame 的视口决定，出图前把所有
            // 相机统一摆回最终锚点。平面模式此分支整体跳过，零开销。
            bool anyGlobe = false;
            for (View3D::OgreScene* scene : std::as_const(m_scenes)) {
                if (scene->globeModeEnabled()) {
                    anyGlobe = true;
                    break;
                }
            }
            if (anyGlobe) {
                for (View3D::OgreViewportSurface* view : std::as_const(m_views)) {
                    if (view->renderActive())
                        view->syncGlobeCamera();
                }
            }
            bool presented = false;
            if (anyActive) {
                if (auto* root = m_runtime.root()) {
                    root->renderOneFrame();
                    presented = true;
                    const std::uint64_t frame =
                        m_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (frame == 1) {
                        std::cerr << "VIEW3D_RENDER_THREAD_FRAME first=1" << std::endl;
                    }
                }
            }
            // 只统计真正出图的周期：这才是用户能感觉到的节拍与可能的输入阻塞。
            const double gateMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cycleStart).count();
            if (presented)
                recordPacing(cycleStart, gateMs);
        }
        ++m_cycleCount;
        if (!m_paceProbe.isEmpty() && m_cycleCount % kPacingSamples == 0) {
            std::cerr << "VIEW3D_RENDER_STATS cycles=" << m_cycleCount << " active="
                      << activeCount << " of " << m_views.size() << std::endl << std::flush;
            printPacingStats("periodic");
        }
    }

    void recordPacing(const std::chrono::steady_clock::time_point& cycleStart, double gateMs)
    {
        if (m_hasLastCycle) {
            const std::size_t index = m_sampleCount % kPacingSamples;
            m_intervalSamples[index] = std::chrono::duration<double, std::milli>(
                cycleStart - m_lastCycleStart).count();
            m_gateSamples[index] = gateMs;
            ++m_sampleCount;
        } else {
            m_hasLastCycle = true;
        }
        m_lastCycleStart = cycleStart;
    }

    static void summarizeSamples(const std::vector<double>& values, const char* label)
    {
        if (values.empty())
            return;
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double value : sorted)
            sum += value;
        const std::size_t p95Index = static_cast<std::size_t>(
            0.95 * static_cast<double>(sorted.size() - 1u));
        std::cerr << " " << label << "_avg=" << (sum / static_cast<double>(sorted.size()))
                  << "_p95=" << sorted[p95Index] << "_max=" << sorted.back();
    }

    void printPacingStats(const char* reason)
    {
        const std::size_t count = qMin<std::size_t>(m_sampleCount, kPacingSamples);
        if (count == 0)
            return;
        std::vector<double> intervals;
        std::vector<double> gateHolds;
        intervals.reserve(count);
        gateHolds.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            intervals.push_back(m_intervalSamples[i]);
            gateHolds.push_back(m_gateSamples[i]);
        }
        std::cerr << "VIEW3D_RENDER_STATS " << reason << " frames="
                  << m_frameCount.load(std::memory_order_relaxed) << " samples=" << count
                  << " intervalMs";
        summarizeSamples(intervals, "interval");
        std::cerr << " gateMs";
        summarizeSamples(gateHolds, "gate");
        std::cerr << " targetFps=" << m_targetFrameRate.load(std::memory_order_relaxed)
                  << std::endl << std::flush;
    }

    void drainCommands()
    {
        std::deque<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            pending.swap(m_commands);
        }
        for (std::function<void()>& command : pending)
            command();
    }

    void waitNextFrame()
    {
        const int intervalMs = m_frameIntervalMs.load(std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(m_commandMutex);
        m_commandCondition.wait_for(lock, std::chrono::milliseconds(intervalMs), [this] {
            return m_stopRequested.load(std::memory_order_relaxed) || !m_commands.empty();
        });
    }

    void shutdownRuntime()
    {
        try {
            std::lock_guard<std::recursive_mutex> ogreGuard(m_runtime.gate());
            m_runtime.shutdown();
        } catch (const std::exception& error) {
            std::cerr << "VIEW3D_RENDER_THREAD_SHUTDOWN_FAILED: " << error.what()
                      << std::endl;
        }
    }

    void joinRenderThread()
    {
        if (!m_renderThread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_stopRequested.store(true, std::memory_order_release);
        }
        m_commandCondition.notify_all();
        m_renderThread.join();
    }

    IApplication* m_application = nullptr;
    View3D::OgreRuntime m_runtime;
    QList<View3D::OgreScene*> m_scenes;
    QList<View3D::OgreViewportSurface*> m_views;
    View3D::OgreScene* m_scene = nullptr;
    std::thread m_renderThread;
    std::mutex m_commandMutex;
    std::condition_variable m_commandCondition;
    std::deque<std::function<void()>> m_commands;
    std::atomic_bool m_stopRequested {false};
    std::atomic_bool m_rendering {false};
    std::atomic_bool m_threadFinished {false};
    std::thread::id m_renderThreadId {};
    std::atomic<double> m_targetFrameRate {60.0};
    std::atomic<int> m_frameIntervalMs {16};
    std::atomic<std::uint64_t> m_frameCount {0};
    static constexpr std::size_t kPacingSamples = 240;
    std::array<double, kPacingSamples> m_intervalSamples {};
    std::array<double, kPacingSamples> m_gateSamples {};
    std::size_t m_sampleCount = 0;
    std::size_t m_cycleCount = 0;
    std::chrono::steady_clock::time_point m_lastCycleStart {};
    bool m_hasLastCycle = false;
    const QString m_paceProbe = qEnvironmentVariable("VIEW3D_RENDER_STATS");
};

void registerRendererFactories(PluginFactoryRegistry* registry)
{
    registry->registerCreator(
        QStringLiteral("plugin.view3d.renderer"), QStringLiteral("extension.renderer"),
        QStringLiteral("extension"),
        [](const PluginCreationContext&) -> IPlugin* { return new OgreRenderService(); });
}

} // namespace

VIEW3D_DEFINE_PLUGIN_ENTRY("plugin.view3d.renderer", registerRendererFactories(registry);)
