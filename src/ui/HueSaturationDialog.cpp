// ============================================================================
//  HueSaturationDialog.cpp
// ============================================================================

#include "ui/HueSaturationDialog.h"

#include "engine/Adjust.h"

#include <QGridLayout>
#include <QSlider>
#include <QVBoxLayout>

namespace zl {

HueSaturationDialog::HueSaturationDialog(const QImage &source, QWidget *parent)
    : AdjustmentDialog(source, tr("Hue/Saturation"), parent)
{
    auto *params = new QWidget(this);
    auto *grid = new QGridLayout(params);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    // 色相绕色环转，一圈 360°，所以取值范围是 [-180, 180]（双向对称）。
    // 饱和度与明度的 [-100, 100] 与"亮度/对比度"保持一致，-100 分别
    // 表示完全去色与完全变黑。
    m_hue        = addSliderRow(grid, 0, tr("Hue"),   -180, 180, 0, tr("°"));
    m_saturation = addSliderRow(grid, 1, tr("Saturation"), -100, 100, 0);
    m_lightness  = addSliderRow(grid, 2, tr("Luminosity"),   -100, 100, 0);

    grid->setColumnStretch(1, 1);

    buildLayout(params);
    resize(460, sizeHint().height());
}

QImage HueSaturationDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return src;

    // 三个参数一次算完。引擎内部在非预乘空间运算，格式往返也由它处理。
    return adjust::applyHueSaturationLightness(src,
                                               m_hue->value(),
                                               m_saturation->value(),
                                               m_lightness->value());
}

bool HueSaturationDialog::isChanged() const
{
    return m_hue->value() != 0
        || m_saturation->value() != 0
        || m_lightness->value() != 0;
}

void HueSaturationDialog::resetParams()
{
    m_hue->setValue(0);
    m_saturation->setValue(0);
    m_lightness->setValue(0);
}

} // namespace zl
