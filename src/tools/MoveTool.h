#pragma once

// ============================================================================
//  MoveTool.h —— 移动工具
// ----------------------------------------------------------------------------
//  功能：拖动当前活动图层的内容。
//
//  ★ 为什么这个工具是 Phase 2 就要写的 ★
//
//    "移动"看似只是"改个偏移量"，但它暴露了一个重要的架构问题：
//    **图层内容的平移会改变图层位图的每一个像素**。
//
//    如果按像素级脏矩形记录撤销数据（像画笔那样），一次拖动就要记录
//    整层的像素 —— 因为内容确实移动到了新位置，旧位置和新位置都是脏的。
//    所以移动工具必须走**整层快照**路线（ImageCommand），而不是 DrawCommand。
//
//    这个选择必须在第一天就定下来：如果移动工具误用 DrawCommand，
//    会出现"拖动 100 像素的位移，撤销栈里躺着上百条命令"的灾难。
//
//  ★ 交互设计（对齐 Photoshop）★
//
//    · 拖动           → 平移图层内容
//    · Shift + 拖动   → 约束为水平或垂直（按拖动方向的主轴判定）
//    · Alt + 拖动     → 复制后移动（原内容留在原地）
//    · 方向键         → 精确移动 1 像素
//    · Shift + 方向键 → 精确移动 10 像素
//
//  ★ 实时预览策略 ★
//
//    拖动过程中**直接修改图层位图**（走 engine/Transform 的 translate），
//    松手时把"拖动前的位图"与"拖动后的位图"打包成一个 ImageCommand。
//
//    为什么不在拖动时用叠加层预览、松手才写入：
//      平移是"整层移动"，用叠加层预览需要把整层内容再画一遍到屏幕上
//      （多一次全图 drawImage），而且缩放/平移视图时叠加层的坐标换算
//      容易出错。直接改位图则复用已有的合成缓存机制，实现简单且性能更好。
//
//    代价：拖动过程中如果程序崩溃，改动会留在文件里。
//      这可以接受 —— 移动工具本来就是"所见即所得"的操作。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QImage>
#include <QPoint>
#include <QPointF>

namespace zl {

class MoveOptions;

class MoveTool : public AbstractTool
{
    Q_OBJECT

public:
    explicit MoveTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~MoveTool() override;

    // ── 身份 ──
    QString id() const override { return QStringLiteral("move"); }
    QString displayName() const override { return tr("移动"); }
    int     shortcutKey() const override { return Qt::Key_V; }

    ToolOptions *options() const override;
    QCursor      cursor() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult keyPress(const ToolEvent &e) override;

    // ── 叠加层 ──
    // 拖动过程中在屏幕上标注"当前偏移量"，让用户知道移了多少。
    void paintScreenOverlay(QPainter &painter, CanvasView *view) override;

    // ── 生命周期 ──
    void commit() override;
    void cancel() override;

private:
    // 开始一次拖动：记录起始状态，准备"拖动前的位图"快照
    void beginDrag(const ToolEvent &e, const QPoint &startImagePos, bool duplicate);

    // 把拖动的当前总位移应用到图层上。
    //
    // ★ 关键实现细节：为什么每次都从"原始位图"重新计算，而不是
    //   在当前位置上累加增量？★
    //
    //     累加增量看起来更省事，但会有两个问题：
    //       ① 反复平移会累积重采样/裁剪误差。虽然整数平移是无损的，
    //          但"移出去再移回来"就会永久丢失边缘内容（超出画布的部分
    //          已经被丢弃了）。
    //       ② 按住 Shift 时约束方向会跳变。用户先水平拖又转向垂直，
    //          累加的方式会把水平位移也算进去，导致最终位置不是用户想要的。
    //
    //     从原始位图重算则总是"所见即所得"：位移 = 当前鼠标 - 起始鼠标，
    //     中间的任何反复移动都不会污染结果。
    //
    //   代价是每次 mouseMove 都要重新平移整层（一次 memcpy）。
    //   4000×3000 的图层约 48MB，memcpy 大约 5~8ms，拖动时够用。
    //   如果实测发现拖动不流畅，可以降级为"拖动时只更新叠加层预览，
    //   松手才真正写入"。
    void applyDragOffset(const QPoint &offset, bool constrain);

    // 把本层当前位图恢复为拖动前的状态
    void restoreOriginalImage();

    // ── 状态 ──

    MoveOptions *m_options = nullptr;

    // 拖动前的图层位图（整层快照）。
    // 只在拖动期间有效，松手或取消后释放 —— 不常驻内存。
    QImage m_originalImage;

    int  m_dragLayerIndex = -1;      // 正在拖动的图层（-1 表示没有）
    bool m_duplicated      = false;  // 本次拖动是否为"复制式拖动"
    bool m_anythingMoved   = false;  // 位移是否非零（用来决定要不要记撤销）
    QPoint m_currentOffset;          // 当前总位移

    // 拖动起点（图像坐标，浮点）
    QPointF m_startImagePos;
};

} // namespace zl
