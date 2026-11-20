#include "LayerGroup.h"

#include <utility>

namespace View3D {

LayerGroup::LayerGroup(QString name) : m_name(std::move(name)) {}

} // namespace View3D
