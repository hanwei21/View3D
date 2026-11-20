#include "WorkSpace.h"
#include "IMessageCenter.h"

namespace RF::PluginEngine {

WorkSpace::WorkSpace(IMessageCenter *messages) : m_messages(messages) {
}

QStringList WorkSpace::selection() const {
    return m_selection;
}

void WorkSpace::setSelection(const QStringList &nodeIds) {
    if (m_selection == nodeIds)
        return;
    m_selection = nodeIds;
    if (m_messages)
        m_messages->publish(QStringLiteral("workspace.selection_changed"),
                            {{QStringLiteral("selection"), nodeIds}});
}

void WorkSpace::beginUpdate() {
    if (++m_updateDepth == 1 && m_messages)
        m_messages->publish(QStringLiteral("workspace.begin_update"));
}

void WorkSpace::endUpdate() {
    if (m_updateDepth > 0 && --m_updateDepth == 0 && m_messages)
        m_messages->publish(QStringLiteral("workspace.end_update"));
}

} // namespace RF::PluginEngine
