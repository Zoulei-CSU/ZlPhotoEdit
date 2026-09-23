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
//  ★★ "撤销存整层" 与 "失效只用一小块" 是两件独立的事 ★★
//
//    上面说的是**撤销数据的粒度**：必须整层（否则撤销数据不完整）。
//    但**画面失效的粒度**完全可以小得多 —— 平移不改变内容、只改变位置，
//    所以画布上真正变了的像素只有两块：
//
//        旧位置的内容包围盒  ∪  新位置的内容包围盒
//
//    这就是"移动图层（拖拽中）→ markDirtyRegion(上帧 ∪ 本帧)"那条规则。
//
//    ★★ 只脏本帧会留残影 ★★
//      用户把图层从左上拖到右下时，只失效"本帧"那块，左上那一块
//      **不会被重新合成**，于是旧位置的内容一直留在画面上。
//      而这个残影只在拖动过程中可见 —— 一松手（那一下是全图失效）
//      就消失得干干净净，属于最难复现的一类。self-check 里有专门一条
//      钉它（tests/move_tool_test.cpp）。
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
#include <QRect>

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

    // ── 本次平移"真正改变了哪一块像素"（图像坐标）──
    //
    //   = 内容包围盒在 from 位置 ∪ 在 to 位置，再与画布求交。
    //
    //   ★ 参数是两个**位置**，不是一个位移量 ★
    //     因为要回答的是"哪两块地方变了"，而不是"移了多远"。
    //     传 (m_currentOffset, 新offset) 就是"上帧 ∪ 本帧"。
    //
    //   返回空矩形 = "算不出来"（图层没有内容 / 两份都在画布外）
    //   → 调用方走全量失效兜底。空在这里是安全的方向。
    QRect movedRegion(const QPoint &from, const QPoint &to) const;

    // ── 状态 ──

    MoveOptions *m_options = nullptr;

    // 拖动前的图层位图（整层快照）。
    // 只在拖动期间有效，松手或取消后释放 —— 不常驻内存。
    QImage m_originalImage;

    // 拖动开始时算一次的"图层内容包围盒"（图像坐标）。
    //
    // ★ 为什么在 beginDrag 里算一次就够 ★
    //   平移是刚体运动：内容包围盒的形状与大小在整个拖动期间不变，
    //   只是跟着位移跑。所以每帧只要 translated() 一下就是 O(1)，
    //   不必每帧重扫一遍整张画布（那是 10ms 量级，会拖垮拖动帧率）。
    //
    // ★ 代价 ★
    //   transform::contentBounds() 要遍历整层像素，4000×3000 约 2~3ms。
    //   只在按下鼠标时发生一次 —— 相比"每帧整图重合成 431ms"可以忽略。
    QRect m_contentBounds;

    int  m_dragLayerIndex = -1;      // 正在拖动的图层（-1 表示没有）
    bool m_duplicated      = false;  // 本次拖动是否为"复制式拖动"
    bool m_anythingMoved   = false;  // 位移是否非零（用来决定要不要记撤销）
    QPoint m_currentOffset;          // 当前总位移

    // 拖动起点（图像坐标，浮点）
    QPointF m_startImagePos;
};

} // namespace zl
