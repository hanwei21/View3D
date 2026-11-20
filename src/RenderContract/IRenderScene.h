#pragma once

#include "ISceneContent.h"
#include "ISceneDisplay.h"

namespace View3D {

// 一个文档一份的共享场景。视口只持有视图状态（相机、窗口、workspace），
// 内容与 GPU 资源由场景统一管理。
class IRenderScene : public ISceneContent, public ISceneDisplay {
  public:
    ~IRenderScene() override = default;

    virtual bool ready() const noexcept = 0;
};

} // namespace View3D
