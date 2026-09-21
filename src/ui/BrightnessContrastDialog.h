#pragma once

// ============================================================================
//  BrightnessContrastDialog.h —— 亮度 / 对比度
// ----------------------------------------------------------------------------
//  两个参数、一条合并 LUT，是"调整"这一类里最简单的一个 ——
//  也正因为简单，它是验证"对话框 + 预览 + 撤销"整条链路是否接通的最佳样本。
//
//  ★ 为什么两个参数要合成一条 LUT ★
//    分开做是 out = contrast(brightness(in))，需要逐像素查两次表
//    （而且中间还要在做完亮度后转回预乘、再做对比度时再转一次）。
//    合成后只查一次表，也不存在"中间格式转换的舍入误差"。
//    顺序固定为**先亮度、后对比度**，与界面上的排列一致（见 Adjust.h）。
// ============================================================================

#include "ui/AdjustmentDialog.h"

namespace zl {

class BrightnessContrastDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    explicit BrightnessContrastDialog(const QImage &source, QWidget *parent = nullptr);

    QImage apply(const QImage &src) const override;
    bool   isChanged() const override;
    void   resetParams() override;

private:
    QSlider *m_brightness = nullptr;
    QSlider *m_contrast   = nullptr;
};

} // namespace zl
