#include "PluginContainer.h"

#include <QMap>

namespace RF {
namespace PluginEngine {

PluginContainer::PluginContainer() {}

PluginContainer::~PluginContainer() {}

void PluginContainer::publishObject(const QString &name, void *object,
                                    const std::type_info &type, IPlugin *owner) {
    if (name.isEmpty() || !object)
        return;
    m_objects.insert(name, object);
    m_objectTypes.insert(name, &type);
    if (owner)
        m_objectOwners.insert(name, owner);
    else
        m_objectOwners.remove(name);
}

void PluginContainer::unpublishObject(const QString &name, void *object) {
    if (m_objects.value(name) != object)
        return;
    m_objects.remove(name);
    m_objectTypes.remove(name);
    m_objectOwners.remove(name);
}

void PluginContainer::unpublishObjectsOwnedBy(IPlugin *owner) {
    if (!owner)
        return;
    for (auto it = m_objectOwners.begin(); it != m_objectOwners.end();) {
        if (it.value() != owner) {
            ++it;
            continue;
        }
        m_objects.remove(it.key());
        m_objectTypes.remove(it.key());
        it = m_objectOwners.erase(it);
    }
}

void *PluginContainer::object(const QString &name) const {
    return m_objects.value(name, nullptr);
}

const std::type_info *PluginContainer::objectType(const QString &name) const {
    return m_objectTypes.value(name, nullptr);
}

// Track plugin state
static QMap<IPlugin*, bool> s_pluginVisible;
static QMap<IPlugin*, bool> s_pluginEnabled;

void PluginContainer::pluginVisible(IPlugin* plugin, bool visible) {
    if (plugin == nullptr) return;
    s_pluginVisible[plugin] = visible;
    if (PluginVisibleChanged.hasHandlers()) {
        PluginVisibleChanged(plugin, visible);
    }
}

void PluginContainer::pluginVisible(const QString& pluginTypeName, bool visible) {
    Q_UNUSED(pluginTypeName);
    Q_UNUSED(visible);
}

bool PluginContainer::pluginIsVisible(IPlugin* plugin) {
    return s_pluginVisible.value(plugin, true);
}

void PluginContainer::pluginEnable(IPlugin* plugin, bool enable) {
    if (plugin == nullptr) return;
    s_pluginEnabled[plugin] = enable;
    if (PluginEnableChanged.hasHandlers()) {
        PluginEnableChanged(plugin, enable);
    }
}

void PluginContainer::pluginEnable(const QString& pluginTypeName, bool enable) {
    Q_UNUSED(pluginTypeName);
    Q_UNUSED(enable);
}

bool PluginContainer::pluginIsEnable(IPlugin* plugin) {
    return s_pluginEnabled.value(plugin, true);
}

void PluginContainer::selectPage(IRibbonPage* ribbonPage) {
    if (SelectedRibbonPageChanged.hasHandlers() && ribbonPage != nullptr) {
        RibbonPageEventArgs args;
        SelectedRibbonPageChanged(this, args);
    }
}

void PluginContainer::setCommandCheckState(ICommand* command, bool checkState) {
    Q_UNUSED(command);
    Q_UNUSED(checkState);
}

void PluginContainer::setCheckBoxState(ICheckBox* checkBox, int newCheckState) {
    Q_UNUSED(checkBox);
    Q_UNUSED(newCheckState);
}

IDockWindow* PluginContainer::createDockWindow(const QString& typeName) {
    IDockWindow* dockWindow = m_dockWindows.value(typeName, nullptr);
    if (dockWindow != nullptr && CreateDockWindowRequested.hasHandlers()) {
        CreateDockWindowRequested(dockWindow);
    }
    return dockWindow;
}

void PluginContainer::destroyDockWindow(IDockWindow* dockWindow) {
    if (dockWindow != nullptr && DestroyDockWindowRequested.hasHandlers()) {
        DestroyDockWindowRequested(dockWindow);
    }
}

void PluginContainer::activeDockWindow(IDockWindow* dockWindow) {
    if (dockWindow != nullptr && ActivateDockWindowRequested.hasHandlers()) {
        ActivateDockWindowRequested(dockWindow);
    }
}

IContentsView* PluginContainer::createContentsView(const QString& typeName, const QString& key) {
    return createContentsView(typeName, key, QString());
}

IContentsView* PluginContainer::createContentsView(const QString& typeName, const QString& key, const QString& caption) {
    IContentsView* contentsView = m_contentsViews.value(typeName, nullptr);
    if (contentsView != nullptr && CreateContentsViewRequested.hasHandlers()) {
        CreateContentsViewRequested(contentsView, key, caption);
    }
    return contentsView;
}

void PluginContainer::activeContentsView(IContentsView* contentsView) {
    if (contentsView != nullptr && ActivateContentsViewRequested.hasHandlers()) {
        ActivateContentsViewRequested(contentsView);
    }
}

void PluginContainer::setContentsViewText(IContentsView* contentsView, const QString& text) {
    if (contentsView != nullptr && SetContentsViewTextRequested.hasHandlers()) {
        SetContentsViewTextRequested(contentsView, text);
    }
}

QString PluginContainer::getContentsViewText(IContentsView* contentsView) {
    QString text;
    if (contentsView != nullptr && GetContentsViewTextRequested.hasHandlers()) {
        GetContentsViewTextRequested(contentsView, text);
    }
    return text;
}

void PluginContainer::closeContentsView(IContentsView* contentsView) {
    if (contentsView != nullptr && CloseContentsViewRequested.hasHandlers()) {
        CloseContentsViewRequested(contentsView);
    }
}

void PluginContainer::setStaticCaption(IStatic* staticPlugin, const QString& caption) {
    Q_UNUSED(staticPlugin);
    Q_UNUSED(caption);
}

void PluginContainer::setProgressValue(IProgress* progress, int value) {
    Q_UNUSED(progress);
    Q_UNUSED(value);
}

void PluginContainer::setGalleryItemCheckState(void* galleryItem, bool checkState) {
    Q_UNUSED(galleryItem);
    Q_UNUSED(checkState);
}

void PluginContainer::performStepProgress(IProgress* progress) {
    Q_UNUSED(progress);
}

void PluginContainer::incrementProgress(IProgress* progress, int value) {
    Q_UNUSED(progress);
    Q_UNUSED(value);
}

void PluginContainer::setEditComboboxText(IEditCombobox* editCombobox, const QString& text) {
    Q_UNUSED(editCombobox);
    Q_UNUSED(text);
}

void PluginContainer::setEditComboboxValue(IEditCombobox* editCombobox, void* value) {
    Q_UNUSED(editCombobox);
    Q_UNUSED(value);
}

int PluginContainer::addComboBoxItem(IEditCombobox* editCombobox, void* comboBoxItem) {
    Q_UNUSED(editCombobox);
    Q_UNUSED(comboBoxItem);
    return -1;
}

void PluginContainer::removeComboBoxItem(IEditCombobox* editCombobox, void* value) {
    Q_UNUSED(editCombobox);
    Q_UNUSED(value);
}

void PluginContainer::clearComboBoxItem(IEditCombobox* editCombobox) {
    Q_UNUSED(editCombobox);
}

void PluginContainer::closeUpDropDownControl(IDropDown* dropDown) {
    Q_UNUSED(dropDown);
}

void PluginContainer::setPluginImage(IPlugin* plugin, void* image) {
    Q_UNUSED(plugin);
    Q_UNUSED(image);
}

void PluginContainer::setPluginCaption(IPlugin* plugin, const QString& caption) {
    Q_UNUSED(plugin);
    Q_UNUSED(caption);
}

} // namespace PluginEngine
} // namespace RF


