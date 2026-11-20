#pragma once

class QMainWindow;

namespace ads {
class CDockWidget;
}

namespace View3DApplication {

// 把测量结果面板收回窗口宽/高的合理比例；保留停靠位置，浮动时限制浮动窗口尺寸。
// 供宿主启动/停靠事件和冒烟检查共用。
void clampMeasurementDock(ads::CDockWidget* dock, QMainWindow* window);

// 把左侧场景树恢复为记录宽度（调整中央视图补差），避免测量面板停靠时左侧被压缩。
void restoreSceneDockWidth(ads::CDockWidget* sceneDock, int width, QMainWindow* window);

} // namespace View3DApplication
