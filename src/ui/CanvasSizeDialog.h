#pragma once

// ============================================================================
//  CanvasSizeDialog.h —— "画布大小"对话框
// ----------------------------------------------------------------------------
//  改的是**容器**：像素一个都不动，不重采样，因此完全无损。
//  变小的部分被裁掉，变大的部分填底色。
//
//  ★ 这个对话框最重要的职责是"别让用户被吓到" ★
//
//    画布缩小时程序会做两件事，而这两件事都不是用户直接要求的：
//      ① 内容可能被裁掉
//      ② 内容可能被**自动平移**（智能适配，见 docs/…第 8.3 节）
//
//    所以界面上必须有：
//      · 一张示意图 —— 实线=新画布、虚线=原画布、蓝块=内容、红色=将被裁掉
//      · 一行文字说明 —— "内容将自动偏移 120 像素以避免被裁切"
//      · 裁切警告 —— 明确写出水平/垂直各被裁掉多少像素
//
//    没有这些，用户看到"我明明只改了个数字，图怎么自己跑到角上去了"，
//    会认定程序有 bug。这不是可选的贴心提示，是正确性的组成部分。
//
//  ★ 偏移量由 engine 的 canvasResizeOffset() 计算，与 Document 共用 ★
//    绝不在对话框里重写一遍"要不要智能适配"的策略 —— 那份策略一旦
//    分叉，就会出现"预览说不会裁，确定后被裁了"这种最糟糕的交互。
// ============================================================================

#include "ui/SizeChangeDialogBase.h"

#include "engine/Geometry.h"

#include <QColor>
#include <QRect>

class QButtonGroup;
class QLabel;
class QToolButton;

namespace zl {

// 示意图控件。实现在 .cpp 里。
//
// ★ 刻意不放在头文件、也不要 Q_OBJECT ★
//   它只被对话框**单向驱动**（对话框调它的 setState()，它自己不发任何
//   信号），所以不需要 moc。少一个带 Q_OBJECT 的头文件，就少一处
//   "忘了加进 .pro 的 HEADERS → 只在链接期报 LNK2001 metaObject" 的机会。
class CanvasPreviewWidget;

class CanvasSizeDialog : public SizeChangeDialogBase
{
    Q_OBJECT

public:
    // contentBounds —— 文档内容的实际包围盒（Document::contentBounds()）。
    //
    // ★ 必须传内容的包围盒，而不是画布矩形 ★
    //   用户在画布上可能有一大圈透明边（例如抠完图贴在角落）。
    //   智能适配要保护的是"看得见的东西"，把透明边也算进去会让
    //   偏移量算得莫名其妙。包围盒为空时传 QRect()。
    CanvasSizeDialog(const QSize &original,
                     const QRect &contentBounds,
                     Anchor initialAnchor = Anchor::Center,
                     QWidget *parent = nullptr);
    ~CanvasSizeDialog() override;

    Anchor anchor() const;
    QColor fillColor() const { return m_fillColor; }

    // 用于状态栏提示与测试断言：内容在新画布中的实际偏移。
    QPoint contentOffset() const;

protected:
    QString infoText(const QSize &newSize) const override;
    void    onSizeEdited(const QSize &newSize) override;

private slots:
    void onAnchorClicked(int id);
    void onPickColor();

private:
    void updatePreview();
    void updateColorButton();

    // 裁切量统计（用于警告文字）。
    struct CropInfo
    {
        int left = 0, right = 0, top = 0, bottom = 0;
        bool any() const { return left > 0 || right > 0 || top > 0 || bottom > 0; }
    };
    CropInfo cropInfo(const QSize &newSize, const QPoint &offset) const;

    QButtonGroup *m_anchorGroup = nullptr;
    QToolButton  *m_colorButton = nullptr;

    QRect  m_contentBounds;
    QColor m_fillColor = Qt::transparent;

    // 示意图。只被本对话框单向驱动，没有自己的信号，因此不必进 moc。
    CanvasPreviewWidget *m_preview = nullptr;
    QLabel *m_legend = nullptr;
};

} // namespace zl
