#pragma once

#include <array>
#include <vector>

namespace View3D {

class IViewportOverlay {
  public:
    virtual ~IViewportOverlay() = default;

    virtual bool setOverlay(const std::vector<std::array<double, 3>>& points,
                            bool connectPoints) = 0;
    virtual void clearOverlay() = 0;
    virtual bool hasOverlay() const noexcept = 0;
};

} // namespace View3D
