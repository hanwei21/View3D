#pragma once

#include "IWorkSpace.h"

namespace RF::PluginEngine {

class IMessageCenter;

class PLUGIN_ENGINE_EXPORT WorkSpace final : public IWorkSpace {
  public:
    explicit WorkSpace(IMessageCenter *messages);
    QStringList selection() const override;
    void setSelection(const QStringList &nodeIds) override;
    void beginUpdate() override;
    void endUpdate() override;

  private:
    IMessageCenter *m_messages = nullptr;
    QStringList m_selection;
    int m_updateDepth = 0;
};

} // namespace RF::PluginEngine
