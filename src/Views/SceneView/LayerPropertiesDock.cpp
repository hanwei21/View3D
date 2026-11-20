#include "LayerPropertiesDock.h"

#include "ILayer.h"
#include "SceneLayerService.h"
#include "SceneTreeCoordinator.h"

#include <IApplication.h>
#include <ISceneContent.h>
#include <View3DObjectIds.h>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <utility>

namespace View3DPlugins {
namespace {

constexpr int kSliderHalf = 100;  // 滑条量程的一半：100 表示系数 1.0

QString loadStateText(View3D::LoadState state)
{
    switch (state) {
    case View3D::LoadState::Unloaded:
        return QStringLiteral("未加载");
    case View3D::LoadState::Queued:
        return QStringLiteral("排队中");
    case View3D::LoadState::Loading:
        return QStringLiteral("加载中");
    case View3D::LoadState::PartiallyReady:
        return QStringLiteral("部分就绪");
    case View3D::LoadState::Ready:
        return QStringLiteral("就绪");
    case View3D::LoadState::Failed:
        return QStringLiteral("失败");
    case View3D::LoadState::Cancelling:
        return QStringLiteral("取消中");
    case View3D::LoadState::Cancelled:
        return QStringLiteral("已取消");
    case View3D::LoadState::Offline:
        return QStringLiteral("离线");
    case View3D::LoadState::SourceInvalid:
        return QStringLiteral("源数据无效");
    }
    return QStringLiteral("未知");
}

QString layerTypeText(View3D::LayerType type)
{
    switch (type) {
    case View3D::LayerType::Group:
        return QStringLiteral("图层组");
    case View3D::LayerType::Model:
        return QStringLiteral("模型图层");
    case View3D::LayerType::Tiled:
        return QStringLiteral("瓦片图层");
    case View3D::LayerType::Imagery:
        return QStringLiteral("影像图层");
    case View3D::LayerType::Feature:
        return QStringLiteral("要素图层");
    default:
        return QStringLiteral("未知");
    }
}

QString statisticsText(const QVariantMap& statistics)
{
    QStringList pairs;
    for (auto it = statistics.constBegin(); it != statistics.constEnd(); ++it) {
        QString value = it.value().toString();
        if (value.size() > 60)
            value = value.left(60) + QStringLiteral("…");
        pairs.append(QStringLiteral("%1=%2").arg(it.key(), value));
    }
    return pairs.join(QStringLiteral("  "));
}

// 参照系明细：源→局部的 4×4 矩阵（行主序）+ 经纬度高程（+ 成果自带的平面原点）。
// 没有参照系的模型只有 "-"；非仿射变换（PROJ 重投影）只给经纬度，没有矩阵。
QString referenceFrameText(const View3D::ILayer::ReferenceFrame& frame)
{
    if (frame.position.isEmpty() && frame.origin.isEmpty() && !frame.hasMatrix)
        return QStringLiteral("-");
    QStringList lines;
    if (frame.hasMatrix) {
        for (int row = 0; row < 4; ++row) {
            QStringList cells;
            for (int column = 0; column < 4; ++column) {
                cells.append(QString::number(frame.matrix[static_cast<std::size_t>(row) * 4 +
                                                          static_cast<std::size_t>(column)],
                                             'g', 6)
                                 .rightJustified(11));
            }
            lines.append(QStringLiteral("%1  %2")
                             .arg(row == 0 ? QStringLiteral("矩阵") : QStringLiteral("    "),
                                  cells.join(QLatin1Char(' '))));
        }
    }
    if (!frame.position.isEmpty())
        lines.append(QStringLiteral("经纬度  %1").arg(frame.position));
    if (!frame.origin.isEmpty())
        lines.append(QStringLiteral("原点  %1").arg(frame.origin));
    return lines.join(QLatin1Char('\n'));
}

QSlider* makeSlider(int minimum, int maximum, int value, QWidget* parent)
{
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    slider->setTickPosition(QSlider::NoTicks);
    return slider;
}

QDoubleSpinBox* makeOffsetBox(QWidget* parent)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setDecimals(3);
    box->setRange(-1e7, 1e7);
    box->setSuffix(QStringLiteral(" m"));
    return box;
}

QDoubleSpinBox* makeDistanceBox(QWidget* parent)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setDecimals(1);
    box->setRange(0.0, 1e9);
    box->setSuffix(QStringLiteral(" m"));
    return box;
}

QGroupBox* makeGroup(const QString& title, QWidget* parent, QFormLayout** form)
{
    auto* group = new QGroupBox(title, parent);
    auto* layout = new QFormLayout(group);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    *form = layout;
    return group;
}

} // namespace

LayerPropertiesDock::LayerPropertiesDock(QString name, QString caption)
    : m_name(std::move(name)), m_caption(std::move(caption))
{
    if (m_caption.isEmpty())
        m_caption = QStringLiteral("图层属性");

    auto* surface = new QWidget;
    auto* outer = new QVBoxLayout(surface);
    outer->setContentsMargins(8, 8, 8, 8);

    m_hint = new QLabel(QStringLiteral("未选中图层"), surface);
    m_hint->setObjectName(QStringLiteral("layerHint"));
    m_hint->setWordWrap(true);
    outer->addWidget(m_hint);

    QFormLayout* form = nullptr;

    auto* identity = makeGroup(QStringLiteral("基本信息"), surface, &form);
    m_nameEdit = new QLineEdit(identity);
    m_nameEdit->setObjectName(QStringLiteral("layerName"));
    form->addRow(QStringLiteral("名称"), m_nameEdit);
    m_typeLabel = new QLabel(identity);
    form->addRow(QStringLiteral("类型"), m_typeLabel);
    m_stateLabel = new QLabel(identity);
    m_stateLabel->setWordWrap(true);
    form->addRow(QStringLiteral("状态"), m_stateLabel);
    m_sourceLabel = new QLabel(identity);
    m_sourceLabel->setWordWrap(true);
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("来源"), m_sourceLabel);
    m_crsLabel = new QLabel(identity);
    m_crsLabel->setObjectName(QStringLiteral("layerSourceCrs"));
    m_crsLabel->setWordWrap(true);
    m_crsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("参照系"), m_crsLabel);
    m_referenceFrameLabel = new QLabel(identity);
    m_referenceFrameLabel->setObjectName(QStringLiteral("layerReferenceFrame"));
    m_referenceFrameLabel->setWordWrap(true);
    m_referenceFrameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 矩阵要按列对齐才看得出轴交换，等宽字体是必须的。
    m_referenceFrameLabel->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    form->addRow(m_referenceFrameLabel);
    outer->addWidget(identity);

    auto* display = makeGroup(QStringLiteral("显示"), surface, &form);
    m_visibleCheck = new QCheckBox(QStringLiteral("显示图层"), display);
    m_visibleCheck->setObjectName(QStringLiteral("layerVisible"));
    m_lockedCheck = new QCheckBox(QStringLiteral("锁定（禁止勾选与拖拽）"), display);
    m_lockedCheck->setObjectName(QStringLiteral("layerLocked"));
    auto* flags = new QWidget(display);
    auto* flagsLayout = new QVBoxLayout(flags);
    flagsLayout->setContentsMargins(0, 0, 0, 0);
    flagsLayout->addWidget(m_visibleCheck);
    flagsLayout->addWidget(m_lockedCheck);
    form->addRow(QStringLiteral("可见性"), flags);
    m_opacity = makeSlider(0, kSliderHalf, kSliderHalf, display);
    m_opacity->setObjectName(QStringLiteral("layerOpacity"));
    m_opacityValue = new QLabel(display);
    auto* opacityRow = new QWidget(display);
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->addWidget(m_opacity, 1);
    opacityLayout->addWidget(m_opacityValue);
    form->addRow(QStringLiteral("不透明度"), opacityRow);
    outer->addWidget(display);

    auto* offset = makeGroup(QStringLiteral("位置偏移"), surface, &form);
    m_offsetX = makeOffsetBox(offset);
    m_offsetX->setObjectName(QStringLiteral("layerOffsetX"));
    m_offsetY = makeOffsetBox(offset);
    m_offsetZ = makeOffsetBox(offset);
    form->addRow(QStringLiteral("X"), m_offsetX);
    form->addRow(QStringLiteral("Y"), m_offsetY);
    form->addRow(QStringLiteral("Z"), m_offsetZ);
    outer->addWidget(offset);

    auto* limits = makeGroup(QStringLiteral("显示距离"), surface, &form);
    m_minDistance = makeDistanceBox(limits);
    m_minDistance->setObjectName(QStringLiteral("layerMinDistance"));
    m_maxDistance = makeDistanceBox(limits);
    m_maxDistance->setSpecialValueText(QStringLiteral("不限"));
    form->addRow(QStringLiteral("最近"), m_minDistance);
    form->addRow(QStringLiteral("最远"), m_maxDistance);
    auto* limitsHint = new QLabel(
        QStringLiteral("瓦片图层的层级切换由数据内嵌阈值决定，此处只对模型图层开放。"),
        limits);
    limitsHint->setWordWrap(true);
    form->addRow(limitsHint);
    outer->addWidget(limits);

    auto* style = makeGroup(QStringLiteral("样式"), surface, &form);
    m_tintR = makeSlider(0, 2 * kSliderHalf, kSliderHalf, style);
    m_tintG = makeSlider(0, 2 * kSliderHalf, kSliderHalf, style);
    m_tintB = makeSlider(0, 2 * kSliderHalf, kSliderHalf, style);
    auto* tintRow = new QWidget(style);
    auto* tintLayout = new QHBoxLayout(tintRow);
    tintLayout->setContentsMargins(0, 0, 0, 0);
    tintLayout->addWidget(m_tintR, 1);
    tintLayout->addWidget(m_tintG, 1);
    tintLayout->addWidget(m_tintB, 1);
    form->addRow(QStringLiteral("色调 R/G/B"), tintRow);
    m_brightness = makeSlider(0, 3 * kSliderHalf, kSliderHalf, style);
    m_contrast = makeSlider(0, 3 * kSliderHalf, kSliderHalf, style);
    form->addRow(QStringLiteral("亮度"), m_brightness);
    form->addRow(QStringLiteral("对比度"), m_contrast);
    m_edgeLines = new QCheckBox(QStringLiteral("显示边线"), style);
    m_edgeLines->setObjectName(QStringLiteral("layerEdgeLines"));
    form->addRow(QStringLiteral("边线"), m_edgeLines);
    outer->addWidget(style);

    auto* stats = makeGroup(QStringLiteral("统计"), surface, &form);
    m_statsLabel = new QLabel(stats);
    m_statsLabel->setObjectName(QStringLiteral("layerStats"));
    m_statsLabel->setWordWrap(true);
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(m_statsLabel);
    outer->addWidget(stats);

    outer->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(surface);
    m_widget = scroll;

    // 面板不是 QObject（工程未启用 moc），一律用「发送者即子控件」的三段式连接：
    // 子控件随面板销毁，连接自动断开。
    QObject::connect(m_nameEdit, &QLineEdit::editingFinished, [this] { commitName(); });
    QObject::connect(m_visibleCheck, &QCheckBox::toggled, [this] { commitVisible(); });
    QObject::connect(m_lockedCheck, &QCheckBox::toggled, [this] { commitLocked(); });
    QObject::connect(m_opacity, &QSlider::valueChanged, [this] {
        m_opacityValue->setText(QStringLiteral("%1%").arg(m_opacity->value()));
        commitOpacity();
    });
    // 数值框按 valueChanged 提交（与滑条一致）：editingFinished 只在真人回车/失焦时才发，
    // 属性面板这类控件逐键生效更符合预期，也让无头检查能驱动同一条路径。
    // reload() 期间 m_loading 为真，回填不会反过来写服务。
    QObject::connect(m_offsetX, &QDoubleSpinBox::valueChanged, [this] { commitOffset(); });
    QObject::connect(m_offsetY, &QDoubleSpinBox::valueChanged, [this] { commitOffset(); });
    QObject::connect(m_offsetZ, &QDoubleSpinBox::valueChanged, [this] { commitOffset(); });
    QObject::connect(m_minDistance, &QDoubleSpinBox::valueChanged, [this] { commitDistance(); });
    QObject::connect(m_maxDistance, &QDoubleSpinBox::valueChanged, [this] { commitDistance(); });
    for (QSlider* slider : { m_tintR, m_tintG, m_tintB, m_brightness, m_contrast }) {
        QObject::connect(slider, &QSlider::valueChanged, [this] { commitStyle(); });
    }
    QObject::connect(m_edgeLines, &QCheckBox::toggled, [this] { commitStyle(); });

    setEditable(false, false, false);
}

LayerPropertiesDock::~LayerPropertiesDock()
{
    onDestroy();
}

void LayerPropertiesDock::onDestroy()
{
    if (m_alive)
        *m_alive = false;
    m_alive.reset();
    m_service = nullptr;
    m_coordinator = nullptr;
}

// getWidget() 在头文件里内联实现（与其它 dock 一致），这里只负责接线。
void LayerPropertiesDock::bind(SceneLayerService& service, SceneTreeCoordinator& coordinator)
{
    m_service = &service;
    m_coordinator = &coordinator;
    m_alive = std::make_shared<bool>(true);

    coordinator.setSelectionListener([alive = m_alive, this](const QList<View3D::LayerId>& ids) {
        if (!*alive)
            return;
        setCurrentLayer(ids.isEmpty() ? View3D::LayerId {} : ids.first());
    });
    // 图层增删改（包括数据插件在别处改名、切可见性）都要回流到面板。
    service.addChangeCallback([alive = m_alive, this](const LayerChange& change) {
        if (!*alive)
            return;
        if (change.id == m_current)
            reload();
        else if (change.kind == LayerChangeKind::Removed && m_current.isNull())
            reload();
    });

    // 初始状态与通知走同一个来源，避免「服务里的选中集合」和「树里真正高亮的行」不一致。
    const QList<View3D::LayerId> observed = coordinator.observerLayerIds();
    setCurrentLayer(observed.isEmpty() ? View3D::LayerId {} : observed.first());
}

void LayerPropertiesDock::setCurrentLayer(const View3D::LayerId& layerId)
{
    m_current = layerId;
    reload();
}

QList<RF::PluginEngine::PublishedService> LayerPropertiesDock::publishedServices() const
{
    QList<RF::PluginEngine::PublishedService> result;
    result.append({View3D::ServiceIds::LayerPropertiesDock,
                   static_cast<View3D::ILayerPropertiesDock *>(const_cast<LayerPropertiesDock *>(this)),
                   &typeid(View3D::ILayerPropertiesDock)});
    return result;
}

std::shared_ptr<View3D::ILayer> LayerPropertiesDock::currentLayerPtr() const
{
    if (!m_service || m_current.isNull())
        return nullptr;
    return m_service->layers().layer(m_current);
}

void LayerPropertiesDock::setEditable(bool layerEditable, bool distanceEditable,
                                      bool styleEditable)
{
    m_nameEdit->setEnabled(layerEditable);
    m_visibleCheck->setEnabled(layerEditable);
    m_lockedCheck->setEnabled(layerEditable);
    m_opacity->setEnabled(layerEditable);
    m_offsetX->setEnabled(layerEditable);
    m_offsetY->setEnabled(layerEditable);
    m_offsetZ->setEnabled(layerEditable);
    m_minDistance->setEnabled(distanceEditable);
    m_maxDistance->setEnabled(distanceEditable);
    m_tintR->setEnabled(styleEditable);
    m_tintG->setEnabled(styleEditable);
    m_tintB->setEnabled(styleEditable);
    m_brightness->setEnabled(styleEditable);
    m_contrast->setEnabled(styleEditable);
    m_edgeLines->setEnabled(styleEditable);
}

void LayerPropertiesDock::reload()
{
    const auto layer = currentLayerPtr();
    m_loading = true;
    if (!layer) {
        m_hint->setText(QStringLiteral("未选中图层：在场景树里点选一个图层或图层组。"));
        m_nameEdit->clear();
        m_typeLabel->setText(QStringLiteral("-"));
        m_stateLabel->setText(QStringLiteral("-"));
        m_sourceLabel->setText(QStringLiteral("-"));
        m_crsLabel->setText(QStringLiteral("-"));
        m_referenceFrameLabel->setText(QStringLiteral("-"));
        m_statsLabel->setText(QStringLiteral("-"));
        setEditable(false, false, false);
        m_loading = false;
        return;
    }

    const bool group = layer->type() == View3D::LayerType::Group;
    const bool tiled = layer->type() == View3D::LayerType::Tiled;
    m_hint->setText(group ? QStringLiteral("图层组：子图层沿用各自的显示设置。")
                          : QStringLiteral("当前图层：%1").arg(layer->name()));
    m_nameEdit->setText(layer->name());
    m_typeLabel->setText(layerTypeText(layer->type()));
    QString state = loadStateText(layer->loadState());
    const View3D::ErrorInfo error = layer->lastError();
    if (!error.code.isEmpty())
        state += QStringLiteral("　%1").arg(error.message.isEmpty() ? error.code : error.message);
    m_stateLabel->setText(state);
    m_sourceLabel->setText(layer->sourceUri().isEmpty() ? QStringLiteral("-")
                                                        : layer->sourceUri());
    // 坐标已按主坐标系归一，源参照系只用于回溯与核对（导入/加载时由数据侧写入）。
    m_crsLabel->setText(layer->sourceCrs().isEmpty() ? QStringLiteral("-")
                                                     : layer->sourceCrs());
    m_referenceFrameLabel->setText(referenceFrameText(layer->referenceFrame()));
    const QString statistics = statisticsText(layer->statistics());
    m_statsLabel->setText(statistics.isEmpty() ? QStringLiteral("-") : statistics);

    m_visibleCheck->setChecked(layer->isVisible());
    m_lockedCheck->setChecked(layer->isLocked());
    m_opacity->setValue(qRound(layer->opacity() * kSliderHalf));
    m_opacityValue->setText(QStringLiteral("%1%").arg(m_opacity->value()));

    std::array<double, 3> offset { 0.0, 0.0, 0.0 };
    if (m_service && !group) {
        const QString key = m_service->renderKey(m_current);
        if (!key.isEmpty() && m_service->scene())
            m_service->scene()->modelLayerOffset(key, &offset);
    }
    m_offsetX->setValue(offset[0]);
    m_offsetY->setValue(offset[1]);
    m_offsetZ->setValue(offset[2]);

    m_minDistance->setValue(static_cast<double>(layer->minimumVisibleDistance()));
    m_maxDistance->setValue(static_cast<double>(layer->maximumVisibleDistance()));

    const std::array<float, 3> tint = layer->styleTint();
    m_tintR->setValue(qRound(tint[0] * kSliderHalf));
    m_tintG->setValue(qRound(tint[1] * kSliderHalf));
    m_tintB->setValue(qRound(tint[2] * kSliderHalf));
    m_brightness->setValue(qRound(layer->styleBrightness() * kSliderHalf));
    m_contrast->setValue(qRound(layer->styleContrast() * kSliderHalf));
    m_edgeLines->setChecked(layer->styleEdgeLines());

    // 可编辑范围跟着服务端能力走：图层组没有自身显示设置，瓦片图层不接受
    // 额外的距离窗口与样式覆盖（这两个 setter 会直接返回 false）。
    setEditable(true, !group && !tiled, !group && !tiled);
    m_loading = false;
}

void LayerPropertiesDock::commitName()
{
    const auto layer = currentLayerPtr();
    if (!layer || m_loading)
        return;
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty() || name == layer->name()) {
        reload();
        return;
    }
    layer->setName(name);
    if (m_coordinator)
        m_coordinator->handleChange(LayerChange { LayerChangeKind::Updated, m_current });
}

void LayerPropertiesDock::commitVisible()
{
    if (m_loading || !m_service)
        return;
    if (!m_service->setLayerVisible(m_current, m_visibleCheck->isChecked()))
        reload();
}

void LayerPropertiesDock::commitLocked()
{
    const auto layer = currentLayerPtr();
    if (!layer || m_loading)
        return;
    layer->setLocked(m_lockedCheck->isChecked());
    if (m_coordinator)
        m_coordinator->handleChange(LayerChange { LayerChangeKind::Updated, m_current });
}

void LayerPropertiesDock::commitOpacity()
{
    if (m_loading || !m_service)
        return;
    const float opacity = static_cast<float>(m_opacity->value()) / kSliderHalf;
    if (!m_service->setLayerOpacity(m_current, opacity))
        reload();
}

void LayerPropertiesDock::commitOffset()
{
    if (m_loading || !m_service)
        return;
    const std::array<double, 3> offset { m_offsetX->value(), m_offsetY->value(),
                                         m_offsetZ->value() };
    if (!m_service->setLayerOffset(m_current, offset))
        reload();
}

void LayerPropertiesDock::commitDistance()
{
    if (m_loading || !m_service)
        return;
    if (!m_service->setLayerVisibleDistance(m_current, m_minDistance->value(),
                                            m_maxDistance->value())) {
        reload();
        return;
    }
    const auto layer = currentLayerPtr();
    if (layer && m_coordinator)
        m_coordinator->handleChange(LayerChange { LayerChangeKind::Updated, m_current });
}

void LayerPropertiesDock::commitStyle()
{
    if (m_loading || !m_service)
        return;
    const std::array<float, 3> tint { static_cast<float>(m_tintR->value()) / kSliderHalf,
                                       static_cast<float>(m_tintG->value()) / kSliderHalf,
                                       static_cast<float>(m_tintB->value()) / kSliderHalf };
    const float brightness = static_cast<float>(m_brightness->value()) / kSliderHalf;
    const float contrast = static_cast<float>(m_contrast->value()) / kSliderHalf;
    if (!m_service->setLayerStyle(m_current, tint, m_edgeLines->isChecked(), brightness, contrast))
        reload();
}

} // namespace View3DPlugins
