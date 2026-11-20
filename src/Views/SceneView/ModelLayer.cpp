#include "ModelLayer.h"

#include <algorithm>
#include <cmath>

namespace View3D {

ModelLayer::ModelLayer(QString name, QString sourceUri, QVariantMap statistics,
                       LayerType type)
    : m_type(type), m_name(std::move(name)), m_sourceUri(std::move(sourceUri)),
      m_statistics(std::move(statistics))
{
    m_transform.setToIdentity();
}

void ModelLayer::setOpacity(float opacity)
{
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void ModelLayer::setVisibleDistance(double minimumDistance, double maximumDistance)
{
    if (!std::isfinite(minimumDistance) || !std::isfinite(maximumDistance) ||
        minimumDistance < 0.0 || maximumDistance < 0.0 ||
        (maximumDistance > 0.0 && maximumDistance < minimumDistance))
        return;
    m_minimumVisibleDistance = minimumDistance;
    m_maximumVisibleDistance = maximumDistance;
    ++m_contentVersion;
}

void ModelLayer::setStyle(const std::array<float, 3>& tint, bool edgeLines,
                          float brightness, float contrast)
{
    for (const float component : tint) {
        if (!std::isfinite(component) || component < 0.0f || component > 2.0f)
            return;
    }
    if (!std::isfinite(brightness) || brightness < -1.0f || brightness > 1.0f ||
        !std::isfinite(contrast) || contrast < 0.5f || contrast > 2.0f)
        return;
    m_styleTint = tint;
    m_styleEdgeLines = edgeLines;
    m_styleBrightness = brightness;
    m_styleContrast = contrast;
    ++m_contentVersion;
}

} // namespace View3D
