#pragma once

#include <QString>
#include "Event.h"

namespace RF {
namespace PluginEngine {

class IDynamicMenuGroup {
public:
    virtual ~IDynamicMenuGroup() = default;

    virtual bool add(const QString& caption, void* value, bool isChecked, void* glyph) = 0;
    virtual bool clear() = 0;
    virtual bool insert(int index, const QString& caption, void* value, bool isChecked, void* glyph) = 0;
    virtual bool remove(void* value) = 0;
    virtual bool removeAt(int index) = 0;
    virtual bool removeRange(int startIndex, int count) = 0;
    virtual bool getChecked(void* value) = 0;
    virtual bool setChecked(void* value, bool isChecked) = 0;
    virtual QString getCaption(void* value) = 0;
    virtual bool setCaption(void* value, const QString& caption) = 0;
    virtual void* getImage(void* value) = 0;
    virtual bool setImage(void* value, void* image) = 0;

    virtual int getCount() const = 0;
    virtual void* getItem(int index) const = 0;
    virtual QString getTitle() const = 0;
    virtual QString getKey() const = 0;
};

class IDynamicMenuManager {
public:
    virtual ~IDynamicMenuManager() = default;

    virtual IDynamicMenuGroup* add(const QString& groupKey, const QString& groupTitle) = 0;
    virtual bool clear() = 0;
    virtual IDynamicMenuGroup* insert(int index, const QString& groupKey, const QString& groupTitle) = 0;
    virtual bool remove(IDynamicMenuGroup* group) = 0;
    virtual bool remove(const QString& groupKey) = 0;
    virtual bool removeAt(int index) = 0;
    virtual bool removeRange(int startIndex, int count) = 0;

    virtual int getCount() const = 0;
    virtual IDynamicMenuGroup* getByKey(const QString& groupKey) const = 0;
    virtual IDynamicMenuGroup* getByIndex(int index) const = 0;
};

} // namespace PluginEngine
} // namespace RF
