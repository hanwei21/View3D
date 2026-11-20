#pragma once

#include <ILayer.h>

namespace View3D {

class LayerGroup final : public ILayer {
public:
    explicit LayerGroup(QString name);

    LayerId id() const override { return m_id; }
    LayerType type() const override { return LayerType::Group; }
    QString name() const override { return m_name; }
    void setName(const QString& name) override { m_name = name; }
    bool isVisible() const override { return m_visible; }
    void setVisible(bool visible) override { m_visible = visible; }
    bool isLocked() const override { return m_locked; }
    void setLocked(bool locked) override { m_locked = locked; }
    bool isSelectable() const override { return false; }
    void setSelectable(bool) override {}
    float opacity() const override { return 1.0f; }
    void setOpacity(float) override {}
    QString sourceUri() const override { return {}; }
    QMatrix4x4 transform() const override { return {}; }
    void setTransform(const QMatrix4x4&) override {}
    LoadState loadState() const override { return LoadState::Ready; }
    ErrorInfo lastError() const override { return {}; }
    std::uint64_t contentVersion() const override { return m_contentVersion; }
    QVariantMap statistics() const override { return {}; }

private:
    LayerId m_id { QUuid::createUuid() };
    QString m_name;
    bool m_visible { true };
    bool m_locked { false };
    std::uint64_t m_contentVersion { 1 };
};

} // namespace View3D
