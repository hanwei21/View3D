#pragma once

#include <QString>
#include "IItem.h"

namespace RF {
namespace PluginEngine {

class Item : public IItem {
private:
    bool m_group;
    QString m_key;
    bool m_showLargeImage;

public:
    Item() : m_group(false), m_showLargeImage(false) {}
    Item(const QString& key) : m_group(false), m_key(key), m_showLargeImage(false) {}
    Item(const QString& key, bool group) : m_group(group), m_key(key), m_showLargeImage(false) {}
    Item(const QString& key, bool group, bool showLargeImage)
        : m_group(group), m_key(key), m_showLargeImage(showLargeImage) {}

    bool getGroup() const override { return m_group; }
    void setGroup(bool group) override { m_group = group; }
    QString getKey() const override { return m_key; }
    void setKey(const QString& key) override { m_key = key; }
    bool getShowLargeImage() const override { return m_showLargeImage; }
    void setShowLargeImage(bool showLargeImage) override { m_showLargeImage = showLargeImage; }
};

} // namespace PluginEngine
} // namespace RF
