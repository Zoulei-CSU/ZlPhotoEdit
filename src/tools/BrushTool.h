#pragma once

// ============================================================================
//  BrushTool.h —— 画笔 / 橡皮
// ----------------------------------------------------------------------------
//  画笔与橡皮共用同一个类，用 Kind 区分。这样做的理由：
//
//    · 两者的**交互逻辑完全相同** —— 都是"按住拖动，沿轨迹落笔"。
//      区别只在最后一步合成：一个是把颜色盖上去，一个是把 alpha 擦掉。
//      拆成两个类会让"补点""蒙版累积""撤销打包"这三块逻辑复制两遍。
//
//    · 参数也基本共用（大小、硬度、不透明度、流量），
//      BrushOptions 本身就带 Kind 字段，橡皮只是换了默认值。
//
//  ★★ 撤销策略：整笔一条命令 ★★
//
//  一次鼠标拖动（按下 → 移动若干次 → 抬起）只产生**一条** DrawCommand。
//  这样用户画一笔后按一次 Ctrl+Z 就退回一笔，符合直觉。
//
//  实现上并没有用 DrawCommand 的 mergeWith 机制，而是用更直接的办法：
//  拖动期间**不碰撤销栈**，把整笔的像素变化攒在工具内部，
//  松手时一次性构造命令。两种方式最终存的数据量是一样的
//  （mergeWith 合并到最后也是"最早 before + 最新 after + 并集矩形"），
//  但一次性构造少了一堆"每步构造-销毁中间命令对象"的开销，
//  也避免了合并逻辑里的边界错误。
//
//  ★★ 为什么"每帧从落笔前的快照重算"，而不是"在上一帧结果上继续画" ★★
//
//  重算看起来浪费，但它是唯一能做到**幂等**的做法：
//
//    · 画笔的合成是 source-over，橡皮是乘法 —— 两者都不可逆。
//      在上一帧结果上继续叠加，误差会随帧数累积。
//      最典型的症状是橡皮反复经过同一处时，"擦不干净"（残留淡影）。
//
//    · 按住鼠标不动也会持续收到 move 事件（微小抖动）。叠加式实现下，
//      手一抖就会不由自主地多画几笔，线条局部变深。
//      重算式实现下，抖动只是重算同一个结果，画面纹丝不动。
//
//  代价是每帧重算的像素数随笔画变长而增加。为此做了两层限制：
//    ① 只重算"本帧新增落笔点所覆盖的矩形"，不是整笔包围盒
//       （落在该矩形外的像素，其覆盖度本帧没变，值仍然正确）
//    ② 重算区域是矩形而不是整幅画布
//  于是常规笔画每帧只处理几千到几万像素，拖动流畅。
// ============================================================================

#include "tools/AbstractTool.h"
#include "engine/Brush.h"

#include <QImage>
#include <QRect>
#include <QPointF>

namespace zl {

class BrushOptions;

class BrushTool : public AbstractTool
{
    Q_OBJECT

public:
    // 画笔 / 橡皮。用枚举而不是"bool isEraser"，
    // 因为将来可能加"背景橡皮""魔术橡皮"，枚举更好扩展。
    enum class Kind { Brush, Eraser };

    BrushTool(Kind kind, Document *doc, CommandStack *stack,
              QObject *parent = nullptr);
    ~BrushTool() override;

    // ── 身份 ──
    Kind kind() const { return m_kind; }

    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    // ★ 用 Qt::BlankCursor + 自绘轮廓，而不是系统箭头 ★
    //
    //   画笔的大小是随时可调的（1~500 像素），用户必须能**提前看到**
    //   落笔范围，否则只能画完了再撤销。系统光标做不到这一点
    //   （QCursor 只支持固定尺寸的位图），所以隐藏系统光标，
    //   在 paintScreenOverlay 里画一个圆。
    //   这是 Photoshop / GIMP / Krita 的共同做法。
    QCursor cursor() const override;

    ToolOptions *options() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult hoverMove(const ToolEvent &e) override;

    // ── 叠加层 ──
    // 画笔刷轮廓圆。**在控件坐标下绘制**（paintScreenOverlay 而非
    // paintOverlay），这样圆的大小在屏幕上恒定：放大到 800% 时，
    // 20 像素的笔刷画出来仍是一个 160 像素的圆，而不是被放大成巨圈。
    void paintScreenOverlay(QPainter &painter, CanvasView *view) override;

    // ── 生命周期 ──
    void commit() override;
    void cancel() override;
    bool isBusy() const override;

    // ── 视图变化 ──
    // 缩放/平移后，缓存下来的控件坐标立刻失效。
    // 工具层拿不到 CanvasView 无法重新换算，所以只把缓存标记为无效
    // （见 m_hoverStale 的说明）。
    void viewChanged() override;

    // ── 前景色同步 ──
    // 画布在"用户改了前景色"与"本工具被激活"两个时机把颜色推过来。
    // 工具不主动去查前景色 —— 那样 tools 层就要反向依赖 ui 层。
    void setForegroundColor(const QColor &color) override;

private:
    // ── 笔画生命周期 ──

    // 开始一笔：取快照、准备蒙版、允许"落笔即画一个点"
    bool beginStroke(const ToolEvent &e);

    // 从 from 到 to 补点并落笔，返回本帧新增落笔点的并集矩形
    // （图像坐标，已与画布求交）。返回空矩形表示本帧没有实际落笔。
    QRect paintSegment(const QPointF &from, const QPointF &to);

    // 把"本帧新增的区域"从落笔前快照重新合成一遍并写回图层
    void applyRegion(const QRect &region);

    // 松手：把整笔打包成一条 DrawCommand 压栈
    void finishStroke();

    // 放弃这一笔（Esc / 切换工具时的 cancel）：
    // 把图层恢复成落笔前的样子，不产生任何撤销记录
    void discardStroke();

    // 当前笔尖（按参数缓存，参数不变就不重建）
    const BrushTip &currentTip();

    // 有效图层的可写位图；返回 nullptr 表示当前不可写
    QImage *writableLayerImage();

    // 实际用于合成的颜色。
    //
    //  画笔颜色来自选项（由画布在"前景色变化"时推入），
    //  但还不能直接用 —— 还要把"不透明度"乘进 alpha：
    //
    //    选项里的 color 是纯色（alpha=255），
    //    不透明度是"整笔最多能盖多少"，两者是独立的两个滑块。
    //    合成时把 alpha 当作"整笔的覆盖上限"最自然：
    //    composeStroke 内部会用 color.alpha() 去缩放覆盖度。
    //
    //  ★ 橡皮忽略颜色 ★
    //    它做的是"把 alpha 乘掉"，颜色的 RGB 不参与运算。
    //    但为了不出现"橡皮的选项里颜色滑块还能调"这种困惑，
    //    选项条会隐藏橡皮的颜色控件（见 BrushOptions::createWidget）。
    QColor strokeColor() const;

    // 笔尖间距（像素）。取直径的 1/4 —— 这是各绘图软件的通用取值：
    // 再密会浪费（同一像素被反复覆盖），再疏会出现明显断点。
    qreal stampSpacing() const;

    // ── 状态 ──

    Kind          m_kind;
    BrushOptions *m_options = nullptr;

    // 笔尖缓存。直径或硬度变化时才重建。
    BrushTip m_tip;
    int      m_tipDiameter  = -1;
    int      m_tipHardness  = -1;

    // ── 笔画进行中的状态 ──

    // 整笔的覆盖度蒙版（Grayscale8，画布尺寸）。
    // 落笔用"取最大"合成，所以同一笔内重叠不会加深。
    QImage m_strokeMask;

    // 本笔已触及区域的并集（图像坐标）。松手时作为 DrawCommand 的脏矩形。
    QRect  m_strokeBounds;

    // 落笔前的图层位图（整层快照）。
    //
    // ★ 这里是浅拷贝，不是深拷贝 ★
    //   `m_originalImage = layer->image();` 只增加一次引用计数，O(1)。
    //   真正的深拷贝发生在之后第一次写图层位图时 —— QImage 的
    //   写时复制（detach）自动完成。所以一笔的开销恰好是
    //   "一次深拷贝"，且是惰性的（用户只是点一下没拖动时，
    //   如果没产生修改就不会拷贝）。
    QImage m_originalImage;

    // 选区快照。笔画进行中用户改不了选区，但每帧去查 Document
    // 会带来无数次不必要的比较，存一份更省事。
    QImage m_selectionMask;

    int m_layerIndex = -1;      // 落笔时的活动图层（-1 表示没有笔画在进行）

    QPointF m_lastStampPos;     // 上一个落笔点（浮点，保留亚像素精度）
    bool    m_hasLastStamp = false;

    // 悬停位置（画笔刷轮廓用）
    //
    // ★ 这里缓存的是**控件坐标**，不是图像坐标 ★
    //
    //   原因见 paintScreenOverlay 的说明：工具层不允许 include ui/CanvasView.h，
    //   所以拿不到 imageToWidget() 与 zoom()。而 ToolEvent 里同时带着
    //   widgetPos 与 viewZoom（这两个字段存在的意义就是给这类"屏幕尺寸恒定
    //   的预览"用的），于是工具只要把事件里的值记下来，
    //   绘制时直接用，完全不需要碰 CanvasView。
    QPointF m_hoverWidgetPos;
    qreal   m_hoverViewZoom = 1.0;

    // 视图变化后，缓存的控件坐标就失效了。
    // 工具无法自行重新换算（需要 CanvasView），所以标记失效、
    // 暂时不画轮廓，等下一次鼠标移动带来新坐标。
    //
    // 取舍：缩放后轮廓会消失一瞬，直到用户动一下鼠标。
    // 相比之下，"轮廓画在错误的屏幕位置"会直接误导用户，严重得多 ——
    // 宁可短暂不显示。
    bool m_hoverStale = true;
};

} // namespace zl
