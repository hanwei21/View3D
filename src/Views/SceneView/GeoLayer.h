#pragma once

#include <ILayer.h>

namespace View3D {

// 地理图层的统一数据模型：影像底图 / 要素图层都用它，仅类型不同。
// 几何由 GeoPlugin 的服务提交到渲染场景，本类只持有元数据与显示状态。
class GeoLayer final : public ILayer {
  public:
    GeoLayer(QString name, QString sourceUri, QVariantMap statistics, LayerType type);

    LayerId id() const override { return m_id; }
    LayerType type() const override { return m_type; }
    QString name() const override { return m_name; }
    void setName(const QString& name) override { m_name = name; }
    bool isVisible() const override { return m_visible; }
    void setVisible(bool visible) override { m_visible = visible; }
    bool isLocked() const override { return m_locked; }
    void setLocked(bool locked) override { m_locked = locked; }
    bool isSelectable() const override { return m_selectable; }
    void setSelectable(bool selectable) override { m_selectable = selectable; }
    float opacity() const override { return m_opacity; }
    void setOpacity(float opacity) override { m_opacity = opacity; }
    QString sourceUri() const override { return m_sourceUri; }
    QMatrix4x4 transform() const override { return m_transform; }
    void setTransform(const QMatrix4x4& transform) override
    {
        m_transform = transform;
        ++m_contentVersion;
    }
    LoadState loadState() const override { return m_loadState; }
    void setLoadState(LoadState state) { m_loadState = state; }
    ErrorInfo lastError() const override { return m_error; }
    std::uint64_t contentVersion() const override { return m_contentVersion; }
    QVariantMap statistics() const override { return m_statistics; }

  private:
    LayerId m_id { QUuid::createUuid() };
    LayerType m_type;
    QString m_name;
    QString m_sourceUri;
    QVariantMap m_statistics;
    QMatrix4x4 m_transform;
    bool m_visible { true };
    bool m_locked { false };
    bool m_selectable { true };
    float m_opacity { 1.0f };
    LoadState m_loadState { LoadState::Loading };
    ErrorInfo m_error;
    std::uint64_t m_contentVersion { 1 };
};

} // namespace View3D
