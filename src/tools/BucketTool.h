#pragma once

// ============================================================================
//  BucketTool.h —— 油漆桶
// ----------------------------------------------------------------------------
//  交互只有一句话：**点一下，填一块**。没有拖动、没有预览、没有多帧状态，
//  所以它是所有工具里撤销策略最简单的 —— 一次点击 = 一条 DrawCommand，
//  连"整笔打包"都不需要。
//
//  ★★ 三个设计决定 ★★
//
//  ① **判据取自活动图层，不取自合成图**
//
//     吸管取的是合成图（所见即所得），油漆桶反过来。原因是这两件事的
//     目标不同：吸管要回答"我看到的这个点是什么颜色"，而油漆桶要回答
//     "在**这一层**上，哪些像素与起点算同一块区域"。
//
//     如果拿合成图算区域、却把颜色写进活动图层，在图层混合模式不是
//     正常、或活动图层是半透明时，填出来的形状会与用户看到的边界
//     对不上（边界是下层内容造成的，上层根本没有那个边界）——
//     用户会看到"填出来的色块溢出了图形的边"。
//
//     代价：在空图层上点击会填满整层。这恰恰是正确行为
//     （"把这一层填成某个颜色"）。
//
//  ② **合成复用画笔那条路径**（engine/Fill.h 的 composeFill → composeStroke）
//
//     于是"选区限制""预乘空间合成""填充不透明度"三条规则在整个工程里
//     只有一份实现，油漆桶与画笔不可能出现行为分叉。
//
//  ③ **命令由"压栈"触发，工具不自己写像素**
//
//     画笔必须自己写像素（拖动期间要实时预览），油漆桶不需要 ——
//     所以直接构造好 before/after 两个 patch，push 进去，
//     由 DrawCommand::redo() 完成真正的写入。少一次重复写，
//     也少一处"忘了 markDirty"的机会。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QColor>

namespace zl {

class BucketOptions;

class BucketTool : public AbstractTool
{
    Q_OBJECT

public:
    BucketTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~BucketTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    ToolOptions *options() const override;

    // 前景色 → 填充色（画布在"用户改了前景色"与"本工具被激活"时推过来）
    void setForegroundColor(const QColor &color) override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;

    // ── 执行一次填充（不依赖事件，自检直接调它）──
    //
    //  返回是否真的产生了改动（false = 区域为空 / 图层不可写 / 没有撤销栈）。
    //  改动的写入由它压入的 DrawCommand 完成。
    //
    //  参数全部取自 options()，所以自检可以先配好选项再调它。
    bool fillAt(const QPoint &imagePos);

private:
    BucketOptions *m_options = nullptr;
};

} // namespace zl
