#include "GeoLayer.h"

#include <utility>

namespace View3D {

GeoLayer::GeoLayer(QString name, QString sourceUri, QVariantMap statistics, LayerType type)
    : m_type(type)
    , m_name(std::move(name))
    , m_sourceUri(std::move(sourceUri))
    , m_statistics(std::move(statistics))
{
}

} // namespace View3D
