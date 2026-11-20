#pragma once

#include <QList>
#include <QString>
#include "IItem.h"

namespace RF {
namespace PluginEngine {

class ButtonGroup : public IItem {
private:
    bool m_group;
    QString m_key;
    bool m_showLargeImage;
    QString m_caption;
    QList<IItem*> m_items;

public:
    ButtonGroup() : m_group(false), m_showLargeImage(false) {}
    ButtonGroup(const QString& caption) : m_group(false), m_showLargeImage(false), m_caption(caption) {}
    ButtonGroup(const QString& caption, const QList<IItem*>& items)
        : m_group(false), m_showLargeImage(false), m_caption(caption), m_items(items) {}
    ButtonGroup(const QString& caption, const QList<IItem*>& items, bool group)
        : m_group(group), m_showLargeImage(false), m_caption(caption), m_items(items) {}

    const QString& getCaption() const { return m_caption; }
    void setCaption(const QString& v) { m_caption = v; }

    const QList<IItem*>& getItems() const { return m_items; }
    void setItems(const QList<IItem*>& v) { m_items = v; }

    bool getGroup() const override { return m_group; }
    void setGroup(bool group) override { m_group = group; }
    QString getKey() const override { return m_key; }
    void setKey(const QString& key) override { m_key = key; }
    bool getShowLargeImage() const override { return m_showLargeImage; }
    void setShowLargeImage(bool showLargeImage) override { m_showLargeImage = showLargeImage; }
};

} // namespace PluginEngine
} // namespace RF
