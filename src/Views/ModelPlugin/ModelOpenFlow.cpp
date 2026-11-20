#include "ModelOpenFlow.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>

namespace View3DModel {
namespace {

// 模型行只列常用格式：assimp/OSG 合起来能读几十种格式，全列出来第一屏反而是
// 生僻格式。未列出的格式仍可由读取器认领，从「所有文件」照常打开。
QStringList commonModelSuffixes()
{
    return {QStringLiteral("glb"), QStringLiteral("gltf"), QStringLiteral("obj"),
            QStringLiteral("fbx"), QStringLiteral("dae"), QStringLiteral("stl"),
            QStringLiteral("ply"), QStringLiteral("3ds"), QStringLiteral("3mf"),
            QStringLiteral("off"), QStringLiteral("osg"), QStringLiteral("osgt"),
            QStringLiteral("ive"), QStringLiteral("flt")};
}

QStringList suffixesForKind(ImportFormatKind kind)
{
    QStringList suffixes;
    for (const IModelFormatReader* reader : modelFormatReaders().all()) {
        if (reader->kind() != kind)
            continue;
        for (const QString& suffix : reader->suffixes()) {
            if (!suffixes.contains(suffix))
                suffixes.push_back(suffix);
        }
    }
    if (kind != ImportFormatKind::Model)
        return suffixes;
    QStringList common;
    for (const QString& suffix : commonModelSuffixes()) {
        if (suffixes.contains(suffix))
            common.push_back(suffix);
    }
    return common;
}

QString kindName(ImportFormatKind kind)
{
    switch (kind) {
    case ImportFormatKind::PointCloud:
        return QStringLiteral("点云文件");
    case ImportFormatKind::Raster:
        // 单波段按 DEM 地形起伏网格、多波段按 DOM 贴图网格，同一行入口覆盖两者。
        return QStringLiteral("栅格 / 地形");
    case ImportFormatKind::Vector:
        return QStringLiteral("矢量数据");
    default:
        return QStringLiteral("模型文件");
    }
}

} // namespace

QString fileFilterForCategory(ImportFormatKind kind)
{
    const QStringList suffixes = suffixesForKind(kind);
    if (suffixes.isEmpty())
        return {};
    QStringList patterns;
    for (const QString& suffix : suffixes)
        patterns.push_back(QStringLiteral("*.") + suffix);
    return QStringLiteral("%1 (%2)").arg(kindName(kind), patterns.join(QLatin1Char(' ')));
}

QString displayNameForCategory(ImportFormatKind kind)
{
    return kindName(kind);
}

bool askImportOptions(QWidget* parent, ImportFormatKind kind, ModelImportOptions* options)
{
    if (!options)
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("导入选项"));
    auto* layout = new QFormLayout(&dialog);

    auto* unit = new QComboBox(&dialog);
    // 「自动」与「米」在读取器里同为 1.0：自动即按数据自带单位当米用。
    unit->addItem(QStringLiteral("自动"), 1.0);
    unit->addItem(QStringLiteral("米"), 1.0);
    unit->addItem(QStringLiteral("毫米"), 0.001);
    unit->addItem(QStringLiteral("厘米"), 0.01);
    unit->addItem(QStringLiteral("千米"), 1000.0);
    const int unitIndex = unit->findData(options->unitScale);
    unit->setCurrentIndex(unitIndex >= 0 ? unitIndex : 0);
    layout->addRow(QStringLiteral("单位"), unit);

    QComboBox* axis = nullptr;
    QComboBox* normals = nullptr;
    QComboBox* nodes = nullptr;
    if (kind == ImportFormatKind::Model) {
        axis = new QComboBox(&dialog);
        axis->addItem(QStringLiteral("自动"), static_cast<int>(ModelSourceUpAxis::Auto));
        axis->addItem(QStringLiteral("Y 向上"), static_cast<int>(ModelSourceUpAxis::YUp));
        axis->addItem(QStringLiteral("Z 向上"), static_cast<int>(ModelSourceUpAxis::ZUp));
        axis->setCurrentIndex(axis->findData(static_cast<int>(options->sourceUpAxis)));
        layout->addRow(QStringLiteral("轴向"), axis);

        normals = new QComboBox(&dialog);
        normals->addItem(QStringLiteral("读取"), static_cast<int>(ModelNormalMode::Read));
        normals->addItem(QStringLiteral("重算"), static_cast<int>(ModelNormalMode::Recalculate));
        normals->addItem(QStringLiteral("翻转"), static_cast<int>(ModelNormalMode::Flip));
        normals->setCurrentIndex(normals->findData(static_cast<int>(options->normalMode)));
        layout->addRow(QStringLiteral("法线"), normals);

        nodes = new QComboBox(&dialog);
        nodes->addItem(QStringLiteral("保留"), static_cast<int>(ModelNodeMode::Preserve));
        nodes->addItem(QStringLiteral("合并"), static_cast<int>(ModelNodeMode::Merge));
        nodes->setCurrentIndex(nodes->findData(static_cast<int>(options->nodeMode)));
        layout->addRow(QStringLiteral("节点"), nodes);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    options->unitScale = unit->currentData().toDouble();
    if (axis)
        options->sourceUpAxis = static_cast<ModelSourceUpAxis>(axis->currentData().toInt());
    if (normals)
        options->normalMode = static_cast<ModelNormalMode>(normals->currentData().toInt());
    if (nodes)
        options->nodeMode = static_cast<ModelNodeMode>(nodes->currentData().toInt());
    return true;
}

} // namespace View3DModel
