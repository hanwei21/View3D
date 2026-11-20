#pragma once

#include "CoreTypes.h"

#include <QtCore/QVariantMap>
#include <QtGui/QMatrix4x4>

#include <array>
#include <cstdint>

namespace View3D {

class ILayer {
public:
    virtual ~ILayer() = default;

    virtual LayerId id() const = 0;
    virtual LayerType type() const = 0;
    virtual QString name() const = 0;
    virtual void setName(const QString& name) = 0;

    virtual bool isVisible() const = 0;
    virtual void setVisible(bool visible) = 0;
    virtual bool isLocked() const = 0;
    virtual void setLocked(bool locked) = 0;
    virtual bool isSelectable() const = 0;
    virtual void setSelectable(bool selectable) = 0;
    virtual float opacity() const = 0;
    virtual void setOpacity(float opacity) = 0;

    virtual QString sourceUri() const = 0;
    virtual QMatrix4x4 transform() const = 0;
    virtual void setTransform(const QMatrix4x4& transform) = 0;
    virtual double minimumVisibleDistance() const { return 0.0; }
    virtual double maximumVisibleDistance() const { return 0.0; }
    virtual void setVisibleDistance(double minimumDistance, double maximumDistance) {
        Q_UNUSED(minimumDistance);
        Q_UNUSED(maximumDistance);
    }
    virtual std::array<float, 3> styleTint() const { return {1.0f, 1.0f, 1.0f}; }
    virtual bool styleEdgeLines() const { return false; }
    virtual float styleBrightness() const { return 0.0f; }
    virtual float styleContrast() const { return 1.0f; }
    virtual void setStyle(const std::array<float, 3>& tint, bool edgeLines,
                          float brightness, float contrast) {
        Q_UNUSED(tint);
        Q_UNUSED(edgeLines);
        Q_UNUSED(brightness);
        Q_UNUSED(contrast);
    }
    virtual QString sourceCrs() const { return {}; }
    virtual void setSourceCrs(const QString& crs) { Q_UNUSED(crs); }

    // 参照系框架的完整描述：数据侧知道"源系怎么变成场景局部"时填进来，属性面板展示。
    // 面板只显示矩阵 + 经纬度（+ 成果自带的平面原点），不写说明文字。
    // matrix 只对仿射变换有意义（3D Tiles 的 ECEF→ENU、倾斜摄影的平移+轴交换）；
    // 经 PROJ 重投影的矢量/栅格是非仿射的，没矩阵，只给 position。
    struct ReferenceFrame {
        bool hasMatrix = false;
        // 行主序 4×4：scene = matrix * source（source 为数据自带坐标）。
        std::array<double, 16> matrix {};
        QString position;  // 经纬度高程："116.39°, 39.91°, 椭球高 0 m"
        QString origin;    // 成果自带的平面原点，如 "SRSOrigin 520000, 2500000, 0 m"
    };
    virtual ReferenceFrame referenceFrame() const { return {}; }
    virtual void setReferenceFrame(const ReferenceFrame& frame) { Q_UNUSED(frame); }

    virtual LoadState loadState() const = 0;
    virtual ErrorInfo lastError() const = 0;
    virtual std::uint64_t contentVersion() const = 0;
    virtual QVariantMap statistics() const = 0;
};

} // namespace View3D
