#pragma once

#include "RenderTypes.h"

#include <QImage>
#include <QSize>

namespace View3D {

class IViewportCapture {
  public:
    virtual ~IViewportCapture() = default;

    virtual QImage captureImage(const QSize& outputSize = QSize(),
                                const CaptureOptions& options = CaptureOptions()) = 0;
    virtual QImage captureImageOffscreen(const QSize& outputSize,
                                         const CaptureOptions& options) = 0;
    virtual QImage sceneSnapshot() = 0;
};

} // namespace View3D
