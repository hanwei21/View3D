#pragma once

#include "PEGlobals.h"
#include <QString>
#include <QStringList>

namespace RF::PluginEngine {

class PLUGIN_ENGINE_EXPORT IWorkSpace {
  public:
    virtual ~IWorkSpace() = default;
    virtual QStringList selection() const = 0;
    virtual void setSelection(const QStringList &nodeIds) = 0;
    virtual void beginUpdate() = 0;
    virtual void endUpdate() = 0;
};

} // namespace RF::PluginEngine
