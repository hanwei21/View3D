#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;
class IItem;

class GalleryItem {
  private:
    QString m_caption;
    QString m_description;
    QString m_groupname;
    void *m_image;
    void *m_hoverimage;
    bool m_checked;
    QString m_toolTip;
    void *m_tag;

  public:
    GalleryItem() : m_image(nullptr), m_hoverimage(nullptr), m_checked(false), m_tag(nullptr) {
    }
    GalleryItem(const QString &caption, void *image, const QString &groupname)
        : m_caption(caption), m_groupname(groupname), m_image(image), m_hoverimage(nullptr),
          m_checked(false), m_tag(nullptr) {
    }
    GalleryItem(const QString &caption, const QString &description, void *image, void *hoverimage,
                const QString &groupname)
        : m_caption(caption), m_description(description), m_groupname(groupname), m_image(image),
          m_hoverimage(hoverimage), m_checked(false), m_tag(nullptr) {
    }
    GalleryItem(const QString &caption, const QString &description, void *image, void *hoverimage,
                const QString &groupname, bool checked)
        : m_caption(caption), m_description(description), m_groupname(groupname), m_image(image),
          m_hoverimage(hoverimage), m_checked(checked), m_tag(nullptr) {
    }
    GalleryItem(const QString &caption, const QString &description, void *image, void *hoverimage,
                const QString &groupname, const QString &tooltip)
        : m_caption(caption), m_description(description), m_groupname(groupname), m_image(image),
          m_hoverimage(hoverimage), m_checked(false), m_toolTip(tooltip), m_tag(nullptr) {
    }
    GalleryItem(const QString &caption, const QString &description, void *image, void *hoverimage,
                const QString &groupname, const QString &tooltip, void *tag)
        : m_caption(caption), m_description(description), m_groupname(groupname), m_image(image),
          m_hoverimage(hoverimage), m_checked(false), m_toolTip(tooltip), m_tag(tag) {
    }

    const QString &getCaption() const {
        return m_caption;
    }
    void setCaption(const QString &v) {
        m_caption = v;
    }
    const QString &getDescription() const {
        return m_description;
    }
    void setDescription(const QString &v) {
        m_description = v;
    }
    const QString &getGroupname() const {
        return m_groupname;
    }
    void setGroupname(const QString &v) {
        m_groupname = v;
    }
    void *getImage() const {
        return m_image;
    }
    void setImage(void *v) {
        m_image = v;
    }
    void *getHoverimage() const {
        return m_hoverimage;
    }
    void setHoverimage(void *v) {
        m_hoverimage = v;
    }
    bool getChecked() const {
        return m_checked;
    }
    void setChecked(bool v) {
        m_checked = v;
    }
    const QString &getToolTip() const {
        return m_toolTip;
    }
    void setToolTip(const QString &v) {
        m_toolTip = v;
    }
    void *getTag() const {
        return m_tag;
    }
    void setTag(void *v) {
        m_tag = v;
    }
};

class IGallery : public IPlugin {
  public:
    virtual ~IGallery() = default;

    virtual bool getAllowHoverImages() const = 0;
    virtual QString getCaption() const = 0;
    virtual int getColumnCount() const = 0;
    virtual bool getDrawImageBackground() const = 0;
    virtual bool getEnabled() const = 0;
    virtual int getHoverImageWidth() const = 0;
    virtual int getHoverImageHeight() const = 0;
    virtual int getImageWidth() const = 0;
    virtual int getImageHeight() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual int getPopupColumnCount() const = 0;
    virtual int getPopupImageWidth() const = 0;
    virtual int getPopupImageHeight() const = 0;
    virtual int getPopupRowCount() const = 0;
    virtual int getRowCount() const = 0;
    virtual bool getShowGroupCaption() const = 0;
    virtual bool getShowItemText() const = 0;
    virtual QString getTooltip() const = 0;

    virtual void itemClick(GalleryItem *item) = 0;
    virtual void onCreate(IApplication *app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF
