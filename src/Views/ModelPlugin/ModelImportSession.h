#pragma once

#include "ModelReadTypes.h"

#include <CoreTypes.h>

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QProgressDialog;
class QMessageBox;
class QWidget;

namespace RF {
namespace PluginEngine {
class IApplication;
}
} // namespace RF

namespace View3DModel {

// 一次导入作业：文件、按内容判定出的读取器与本次批次的选项。
struct ModelImportJob {
    QString path;
    QString readerId;
    ImportFormatKind kind = ImportFormatKind::Model;
    ModelImportOptions options;
    View3D::LayerId parentGroup;
    // 动态投影重放作业：renderKey 非空表示「替换已有图层的几何」，不再新建图层。
    // path/readerId 留空时用网格上的源坐标缓存在工作线程重算；缓存超出上限的图层
    // 带上这两项，回头重新读取源文件（结果一样，只是慢）。
    QString renderKey;
    View3D::LayerId replaceLayer;
    // 球面重烘焙：把当前图层几何重建到 ECEF（renderKey 必须非空）。
    bool globeBake = false;
};

// 已完成待落图层的成果；读取在工作线程做，登记图层只能在主线程做。
struct ModelImportOutcome {
    QString path;
    QString readerId;
    ImportFormatKind kind = ImportFormatKind::Model;
    View3D::LayerId parentGroup;
    ModelReadResult result;
    // 本次作业用的导入选项：重放超出缓存上限的图层时要按同样的参数重读源文件。
    ModelImportOptions options;
    // 重放作业原样带回，commit 据此决定是换几何还是新增图层。
    QString renderKey;
    View3D::LayerId replaceLayer;
    bool globeBake = false;
};

// 模型导入会话：排队 + 单工作线程读取 + 主线程落图层。外壳没有任务管理服务，
// 因此线程与轮询都由本插件自己管；同一时刻只有一个文件在解析，避免 GDAL/OSG
// 的驱动注册表和内存预算被并发放大。
class ModelImportSession final {
  public:
    explicit ModelImportSession(RF::PluginEngine::IApplication* application);
    ~ModelImportSession();

    bool start();
    void stop();

    // 已经判定好读取器的单个文件（场景树「打开数据」按打开器的过滤器选中时使用）。
    bool submitWithReader(const QString& path, const QString& readerId,
                          const ModelImportOptions& options, const View3D::LayerId& parentGroup,
                          QString* error);

    // 显示参照系切换：给每个带源地理坐标缓存的模型图层排一个重放作业，缓存超上限的
    // 改走「重新读取源文件」。没有可重放的图层时返回 false 且不开进度框。
    bool submitReprojection(const QString& displayCrs, QString* error);

    // 球面模式切换：进入时把模型图层重烘焙到 ECEF（有源地理坐标缓存的按经纬度，
    // 没有的钉在 (0°,0°) 地表）；退出时恢复平面几何（有缓存的重放，没有的用
    // 烘焙前保留的原网格）。没有可处理的图层时返回 false。
    bool submitGlobeRebuild(bool enabled, QString* error);

  private:
    bool enqueue(ModelImportJob job, QString* error);
    // 进入球面后给单个图层排重烘焙作业（新导入的图层也要走一次）。
    bool enqueueGlobeBake(const QString& renderKey, const View3D::LayerId& layer);
    void poll();
    void workerMain();
    void commit(const ModelImportOutcome& outcome);
    // 按网格自带的源地理坐标缓存重算局部坐标（工作线程执行）。
    ModelReadResult replayCachedMesh(const ModelImportJob& job) const;
    // 把图层当前网格重建到 ECEF（工作线程执行）：
    // 顶点 → 相对本图层 ECEF 原点的偏移，法线旋转到 ECEF 轴。
    ModelReadResult buildGlobeMesh(const ModelImportJob& job) const;
    void log(const QString& message) const;

    // 进度与失败提示都在主线程做：排队即出现，空闲即收起，失败按批次汇总一次。
    QWidget* dialogParent() const;
    void beginBatch();
    void refreshProgress();
    void endBatch();
    void cancelQueued();
    void showFailures();

    RF::PluginEngine::IApplication* m_application = nullptr;
    std::unique_ptr<QTimer> m_pollTimer;
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<ModelImportJob> m_queue;
    std::vector<ModelImportOutcome> m_outcomes;
    QString m_activePath;
    bool m_running = false;
    bool m_cancel = false;
    int m_active = 0;
    int m_layerSequence = 0;

    // 图层重新读取源文件所需的信息：源坐标缓存超上限时，重放只能回头重读。
    struct LayerSource {
        QString path;
        QString readerId;
        ModelImportOptions options;
    };
    QHash<QString, LayerSource> m_layerSources;
    // 无源地理坐标缓存的图层烘焙到球面前的原网格（shared_ptr 共享，不复制几何）：
    // 退出球面时直接换回，省一次重读源文件。
    QHash<QString, std::shared_ptr<const View3D::ModelMeshDto>> m_flatFallback;

    QProgressDialog* m_progress = nullptr;
    QMessageBox* m_failureBox = nullptr;
    QElapsedTimer m_batchClock;
    QStringList m_batchFailures;
    QString m_batchLabel;
    // 进度框同样用于投影重放，标签文案要区分「打开数据」与「按新投影重放」。
    bool m_batchReplay = false;
    int m_batchTotal = 0;
    int m_batchDone = 0;
    bool m_batchCancelled = false;
};

} // namespace View3DModel
