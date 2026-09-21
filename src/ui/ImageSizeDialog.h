#pragma once

// ============================================================================
//  ImageSizeDialog.h —— "图像大小"对话框
// ----------------------------------------------------------------------------
//  改的是**像素数量**：内容跟着一起缩放，需要重采样，图像会变软或变锐。
//  与"画布大小"的区别见 docs/画布与文件操作设计.md 第 0.1 节 ——
//  这两个概念被混为一谈是图片编辑器里最常见的用户误解，所以两个对话框
//  的信息面板都要把"将要发生什么"说清楚。
//
//  ★ 本对话框不预览图像，这是刻意的 ★
//
//    尺寸类操作不是"参数 → 像素映射"（那是调整类对话框的模型），而是
//    "换一份完全不同的位图"。要做实时预览只有两条路：
//      ① 真的缩放一次 —— 4000×3000 跑双三次约 180ms，拖动数字时卡死
//      ② 在降采样副本上缩放 —— 但副本被放回去显示时又要插值一次，
//         用户看到的是"两次重采样的结果"，与实际结果不符，反而误导
//    所以这里只给出**数字**（当前 → 新、像素总量、缩放百分比）与
//    放大/超内存的**风险提示**。Photoshop 的"图像大小"默认也不勾预览，
//    这是同一个理由。
//
//  ★ 重采样方式只在"确定"时起作用 ★
//    因此把控件留在界面上是安全的 —— 换算法不会触发任何运算。
// ============================================================================

#include "ui/SizeChangeDialogBase.h"

#include "core/ImageTypes.h"

class QComboBox;

namespace zl {

class ImageSizeDialog : public SizeChangeDialogBase
{
    Q_OBJECT

public:
    // layerCount —— 文档当前的图层数，仅用于估算新尺寸的内存占用
    //               （所有图层都会被重采样，代价是逐层累加的）。
    ImageSizeDialog(const QSize &original,
                    ResampleMethod initialMethod = ResampleMethod::Bicubic,
                    int layerCount = 1,
                    QWidget *parent = nullptr);
    ~ImageSizeDialog() override;

    ResampleMethod method() const;

    int layerCount() const { return m_layerCount; }

protected:
    QString infoText(const QSize &newSize) const override;

private:
    QComboBox *m_methodCombo = nullptr;
    int        m_layerCount  = 1;
};

} // namespace zl
