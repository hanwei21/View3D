#pragma once

#include <array>

namespace View3D {

enum class StandardCameraView { Front, Back, Left, Right, Top, Bottom, Isometric };
enum class CameraProjectionMode { Perspective, Orthographic };

struct ViewportCameraState {
    std::array<double, 3> position {};
    std::array<double, 3> direction {};
    std::array<double, 3> up {};
    std::array<double, 3> target {};
    std::array<double, 2> viewport {};
    double horizontalFovRadians = 0.0;
    double verticalFovRadians = 0.0;
    bool orthographic = false;
    double orthographicHeight = 0.0;

    bool operator==(const ViewportCameraState& other) const noexcept
    {
        return position == other.position && direction == other.direction &&
            up == other.up && target == other.target && viewport == other.viewport &&
            horizontalFovRadians == other.horizontalFovRadians &&
            verticalFovRadians == other.verticalFovRadians &&
            orthographic == other.orthographic &&
            orthographicHeight == other.orthographicHeight;
    }
    bool operator!=(const ViewportCameraState& other) const noexcept
    {
        return !(*this == other);
    }
};

struct ViewportClippingState {
    bool enabled = false;
    bool inverted = false;
    bool helperVisible = true;
    std::array<double, 3> normal {0.0, 0.0, 1.0};
    double offset = 0.0;
};

struct NavigationSettings {
    double orbitDegreesPerPixel = 0.35;
    double panSensitivity = 1.0;
    double zoomSensitivity = 1.0;
    double keyboardMoveSpeed = 1.0;
    bool invertOrbitHorizontal = false;
    bool invertOrbitVertical = false;
};

} // namespace View3D
