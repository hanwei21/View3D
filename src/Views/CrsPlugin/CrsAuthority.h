#pragma once

#include "CrsBackend.h"

#include <ICrsBrowser.h>

#include <QHash>
#include <QList>
#include <QString>

namespace View3DCrs {

// ICrsBrowser 的实现：EPSG 权威库的分组列表与详情文本。
// 分组照 PROJ 给的投影方法名来，只把「横轴墨卡托」这一大类按官方名的写法再切开
// （UTM 分区 / 高斯-克吕格带号 / 高斯-克吕格中央经线 / 其它）：本工程挑参照系就是
// 在挑带号和中央经线，3845 条混在一组里没法用。零碎方法合并成「其它投影方法」。
class CrsAuthority final : public View3D::ICrsBrowser {
  public:
    explicit CrsAuthority(const CrsBackend& backend);

    const QList<Group>& groups() override;
    bool available() const override;
    QString statusText() const override;
    bool item(const QString& key, Item* out) const override;
    bool info(const QString& definition, Info* out) const override;
    QString lastError() const override;

  private:
    void load();

    const CrsBackend& m_backend;
    QList<Group> m_groups;
    QHash<QString, Item> m_byKey;
    bool m_loaded = false;
    QString m_status;
    mutable QString m_lastError;
};

} // namespace View3DCrs
