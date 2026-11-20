#pragma once
#include <QString>
class QAction;
class QWidget;
namespace RF::PluginEngine {
class IUiObjectRegistry {
public:
    virtual ~IUiObjectRegistry() = default;
    virtual QAction* action(const QString& objectId) const = 0;
    virtual QWidget* widget(const QString& objectId) const = 0;
    virtual void registerAction(const QString& objectId, QAction* action) = 0;
    virtual void registerWidget(const QString& objectId, QWidget* widget) = 0;
    virtual void unregisterAction(const QString& objectId, QAction* action) = 0;
    virtual void unregisterWidget(const QString& objectId, QWidget* widget) = 0;
};
}
