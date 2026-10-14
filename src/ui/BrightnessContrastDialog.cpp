// ============================================================================
//  BrightnessContrastDialog.cpp
// ============================================================================

#include "ui/BrightnessContrastDialog.h"

#include "engine/Adjust.h"
#include "engine/Lut.h"

#include <QGridLayout>
#include <QSlider>
#include <QVBoxLayout>

namespace zl {

BrightnessContrastDialog::BrightnessContrastDialog(const QImage &source, QWidget *parent)
    : AdjustmentDialog(source, tr("Brightness/Contrast"), parent)
{
    auto *params = new QWidget(this);
    auto *grid = new QGridLayout(params);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    // 范围取 [-100, 100]，0 为不变 —— 这是 Photoshop 的区间，
    // 用户对"亮度 +30"该有多亮有直觉，换成 0..200 反而要重新适应。
    m_brightness = addSliderRow(grid, 0, tr("Brightness"), -100, 100, 0);
    m_contrast   = addSliderRow(grid, 1, tr("Contrast"), -100, 100, 0);

    grid->setColumnStretch(1, 1);

    buildLayout(params);
    resize(460, sizeHint().height());
}

QImage BrightnessContrastDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return src;

    const CurveLut lut = adjust::brightnessContrastLut(m_brightness->value(),
                                                       m_contrast->value());

    // applyLutRgb 内部已经处理了"预乘 → 非预乘 → 查表 → 预乘"的往返
    // （见 Lut.h 的预乘陷阱说明），这里不需要也不应该再关心格式。
    return applyLutRgb(src, lut);
}

bool BrightnessContrastDialog::isChanged() const
{
    return m_brightness->value() != 0 || m_contrast->value() != 0;
}

void BrightnessContrastDialog::resetParams()
{
    m_brightness->setValue(0);
    m_contrast->setValue(0);
}

} // namespace zl
