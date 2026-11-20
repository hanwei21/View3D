#pragma once

#include "IRenderScene.h"
#include "IRenderSurface.h"

#include <cstdint>

class QWidget;

namespace View3D {

class IRenderService {
  public:
    virtual ~IRenderService() = default;

    virtual IRenderScene* createScene() = 0;
    virtual void destroyScene(IRenderScene* scene) = 0;
    virtual IRenderSurface* createView(IRenderScene* scene, QWidget* parent = nullptr) = 0;
    virtual void destroyView(IRenderSurface* view) = 0;
    virtual void setTargetFrameRate(double framesPerSecond) = 0;
    virtual double targetFrameRate() const noexcept = 0;
    virtual std::uint64_t frameCount() const noexcept = 0;
};

} // namespace View3D
