#pragma once

// ============================================================================
//  DocumentEnhanceDialog.h —— 文档增强（去阴影 / 白底黑字）
// ----------------------------------------------------------------------------
//  拍纸质文件的第二步：把灰蒙蒙、有阴影的照片提成"白纸黑字"。
//  （第一步是透视校正，见 tools/PerspectiveTool。）
//
//  ★ 为什么它继承 AdjustmentDialog、而不是自己弹个窗口 ★
//    用户要的是"拖滑块时画面立刻跟着变"，而这套"降采样预览 + 确定时
//    对全尺寸原图重算 + 一条撤销记录 + 取消不留痕"的管线已经在
//    AdjustmentDialog / MainWindow::runAdjustment 里了。
//    自己写一遍就会漏掉其中某一环（最常漏的是"取消后要还原"），
//    而且预览与最终结果会走两段不同的代码 —— 那是"预览一个样、
//    确定后另一个样"这类 bug 的唯一来源。
//
//  ★ 纯函数 apply(src) 的另一份红利 ★
//    对话框不认识 Document，所以"批量对一组图套同一参数"是免费的：
//    把参数存下来，对每张图调一次 scan::enhance 即可。
// ============================================================================

#include "ui/AdjustmentDialog.h"

#include "engine/DocumentScan.h"

class QCheckBox;
class QSlider;

namespace zl {

class DocumentEnhanceDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    explicit DocumentEnhanceDialog(const QImage &source, QWidget *parent = nullptr);

    QImage apply(const QImage &src) const override;
    bool   isChanged() const override;
    void   resetParams() override;

    // 供自检读取当前参数（不经过控件，直接反映"确定时会用的那一组"）
    scan::EnhanceParams params() const;

private:
    QSlider   *m_shadow   = nullptr;
    QSlider   *m_contrast = nullptr;
    QSlider   *m_whiten   = nullptr;
    QCheckBox *m_gray     = nullptr;
};

} // namespace zl
