#pragma once

// ============================================================================
//  HueSaturationDialog.h —— 色相 / 饱和度 / 明度
// ----------------------------------------------------------------------------
//  三个参数都**不是**逐通道独立的运算（色相旋转要绕灰度轴转、
//  饱和度要在原色与灰度之间插值），因此不能各自烘一条 LUT 查表。
//  引擎侧统一由 adjust::applyHueSaturationLightness() 一次算完，
//  顺序固定为**先色相、再饱和度、最后明度**，与 Photoshop 一致
//  （顺序换了结果不同，必须与文档约定保持一致）。
//
//  这里特意把三个参数放在同一个对话框里，而不是拆成三个：
//    · 它们是同一次色彩调整的三个维度，分开做用户要来回切三次对话框，
//      每次都要重新读一遍上一轮的结果
//    · 一次算完比分三次算快得多（省掉两次格式转换与两遍全图遍历）
// ============================================================================

#include "ui/AdjustmentDialog.h"

namespace zl {

class HueSaturationDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    explicit HueSaturationDialog(const QImage &source, QWidget *parent = nullptr);

    QImage apply(const QImage &src) const override;
    bool   isChanged() const override;
    void   resetParams() override;

private:
    QSlider *m_hue        = nullptr;
    QSlider *m_saturation = nullptr;
    QSlider *m_lightness  = nullptr;
};

} // namespace zl
