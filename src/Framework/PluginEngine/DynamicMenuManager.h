#pragma once

#include <vector>
#include <QString>
#include "PEGlobals.h"
#include "IDynamicMenuManager.h"
#include "Event.h"

namespace RF {
namespace PluginEngine {

using DynamicMenuClickEvent = Event<void*, void*>;

class PLUGIN_ENGINE_EXPORT DynamicMenuGroup : public IDynamicMenuGroup {
private:
    struct DynamicMenuItem {
        QString caption;
        void* value;
        bool checked;
        void* glyph;
        DynamicMenuItem(const QString& c, void* v, bool chk, void* g)
            : caption(c), value(v), checked(chk), glyph(g) {}
    };

    std::vector<DynamicMenuItem> m_items;
    QString m_key;
    QString m_title;

public:
    DynamicMenuClickEvent DynamicMenuClick;

    DynamicMenuGroup(const QString& key, const QString& title)
        : m_key(key), m_title(title) {}

    bool add(const QString& caption, void* value, bool isChecked, void* glyph) override;
    bool clear() override;
    bool insert(int index, const QString& caption, void* value, bool isChecked, void* glyph) override;
    bool remove(void* value) override;
    bool removeAt(int index) override;
    bool removeRange(int startIndex, int count) override;
    bool getChecked(void* value) override;
    bool setChecked(void* value, bool isChecked) override;
    QString getCaption(void* value) override;
    bool setCaption(void* value, const QString& caption) override;
    void* getImage(void* value) override;
    bool setImage(void* value, void* image) override;

    int getCount() const override { return static_cast<int>(m_items.size()); }
    void* getItem(int index) const override;
    QString getTitle() const override { return m_title; }
    QString getKey() const override { return m_key; }

    void notify(void* value) {
        if (DynamicMenuClick.hasHandlers())
            DynamicMenuClick(this, value);
    }
};

class PLUGIN_ENGINE_EXPORT DynamicMenuManager : public IDynamicMenuManager {
private:
    std::vector<DynamicMenuGroup*> m_groups;

public:
    DynamicMenuManager() = default;
    ~DynamicMenuManager() override;

    IDynamicMenuGroup* add(const QString& groupKey, const QString& groupTitle) override;
    bool clear() override;
    IDynamicMenuGroup* insert(int index, const QString& groupKey, const QString& groupTitle) override;
    bool remove(IDynamicMenuGroup* group) override;
    bool remove(const QString& groupKey) override;
    bool removeAt(int index) override;
    bool removeRange(int startIndex, int count) override;

    int getCount() const override { return static_cast<int>(m_groups.size()); }
    IDynamicMenuGroup* getByKey(const QString& groupKey) const override;
    IDynamicMenuGroup* getByIndex(int index) const override;
};

} // namespace PluginEngine
} // namespace RF
