#pragma once

#include <ILayer.h>

namespace View3D {

class ModelLayer final : public ILayer {
public:
    ModelLayer(QString name, QString sourceUri, QVariantMap statistics,
               LayerType type = LayerType::Model);

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
    void setOpacity(float opacity) override;
    QString sourceUri() const override { return m_sourceUri; }
    QMatrix4x4 transform() const override { return m_transform; }
    void setTransform(const QMatrix4x4& transform) override { m_transform = transform; ++m_contentVersion; }
    double minimumVisibleDistance() const override { return m_minimumVisibleDistance; }
    double maximumVisibleDistance() const override { return m_maximumVisibleDistance; }
    void setVisibleDistance(double minimumDistance, double maximumDistance) override;
    std::array<float, 3> styleTint() const override { return m_styleTint; }
    bool styleEdgeLines() const override { return m_styleEdgeLines; }
    float styleBrightness() const override { return m_styleBrightness; }
    float styleContrast() const override { return m_styleContrast; }
    void setStyle(const std::array<float, 3>& tint, bool edgeLines,
                  float brightness, float contrast) override;
    QString sourceCrs() const override { return m_sourceCrs; }
    void setSourceCrs(const QString& crs) override {
        m_sourceCrs = crs.trimmed();
        ++m_contentVersion;
    }
    ReferenceFrame referenceFrame() const override { return m_referenceFrame; }
    void setReferenceFrame(const ReferenceFrame& frame) override {
        m_referenceFrame = frame;
        ++m_contentVersion;
    }
    LoadState loadState() const override { return m_loadState; }
    void setLoadState(LoadState state) { m_loadState = state; }
    ErrorInfo lastError() const override { return m_error; }
    std::uint64_t contentVersion() const override { return m_contentVersion; }
    QVariantMap statistics() const override { return m_statistics; }
    // 重放投影后顶点/索引数量会变，统计行要跟着刷新，属性面板才不会显示旧数字。
    void setStatistics(const QVariantMap& statistics) {
        m_statistics = statistics;
        ++m_contentVersion;
    }

private:
    LayerId m_id { QUuid::createUuid() };
    LayerType m_type;
    QString m_name;
    QString m_sourceUri;
    QVariantMap m_statistics;
    QMatrix4x4 m_transform;
    double m_minimumVisibleDistance = 0.0;
    double m_maximumVisibleDistance = 0.0;
    std::array<float, 3> m_styleTint {1.0f, 1.0f, 1.0f};
    bool m_styleEdgeLines = false;
    float m_styleBrightness = 0.0f;
    float m_styleContrast = 1.0f;
    QString m_sourceCrs;
    ReferenceFrame m_referenceFrame;
    ErrorInfo m_error;
    LoadState m_loadState { LoadState::Ready };
    bool m_visible { true };
    bool m_locked { false };
    bool m_selectable { true };
    float m_opacity { 1.0f };
    std::uint64_t m_contentVersion { 1 };
};

} // namespace View3D
