#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

class ComboBoxItem {
private:
    QString m_description;
    void* m_value;

public:
    ComboBoxItem(void* value) : m_value(value) {}
    ComboBoxItem(const QString& description, void* value) : m_description(description), m_value(value) {}

    const QString& getDescription() const { return m_description; }
    void setDescription(const QString& description) { m_description = description; }

    void* getValue() const { return m_value; }
    void setValue(void* value) { m_value = value; }
};

class IEditCombobox : public IPlugin {
public:
    virtual ~IEditCombobox() = default;

    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual int getDropDownRows() const = 0;
    virtual bool getEditable() const = 0;
    virtual bool getEnabled() const = 0;
    virtual bool getIsDropDown() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual QString getTooltip() const = 0;
    virtual int getWidth() const = 0;

    virtual void editValueChanged(void* value) = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF

