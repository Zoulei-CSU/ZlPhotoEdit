#pragma once

// ============================================================================
//  SelectionTool.h —— 矩形选框 / 椭圆选框
// ----------------------------------------------------------------------------
//  一个类两种形状（Kind），理由与 BrushTool 一样：交互逻辑完全一致，
//  只有"最后把路径画成矩形还是椭圆"这一行之差。拆成两个类会让
//  "拖动、修饰键换算、预览、撤销打包"这四块复制两遍。
//
//  ★★ 最重要的设计决定：拖动过程中**不碰 Document** ★★
//
//    画笔工具在拖动时会直接写图层位图（那是"所见即所得"的预览方式），
//    但选区工具不能这么做，原因有两个：
//
//      ① 选区不是像素数据，它没有"局部写回"的概念。
//         每次鼠标移动都真的去更新 Document 的选区蒙版，
//         就要为每一次移动都构造一条撤销命令 —— 拖一下鼠标
//         往撤销栈里塞上百条记录，用户按 Ctrl+Z 要按到手酸。
//
//      ② 选区蒙版的分配与布尔运算都不便宜（4000×3000 是 12MB，
//         一次 applyMask 要遍历 1200 万像素）。鼠标每秒发 60~120 次
//         移动事件，根本来不及算。
//
//    所以这里采用"轻量预览 + 收尾一次性提交"：
//      拖动期间 —— 只在成员里记一个 m_previewRect，paintOverlay 时
//                  画个虚线框，完全不产生任何数据结构
//      松手时   —— 才把形状渲成蒙版、做羽化、与已有选区做布尔运算，
//                  然后包成**一条** SelectionCommand 压栈
//
//    这也意味着"松手前按 Esc 取消"是免费的：什么都不用回滚，
//    丢掉 m_previewRect 就行。
//
//  ★★ 修饰键的语义（对齐 Photoshop）★★
//
//    按下鼠标的**那一瞬间**的修饰键决定"运算方式"：
//        Shift              → 添加（并集）
//        Alt                → 减去（差集）
//        Shift + Alt        → 交集
//        无修饰键           → 用选项条上的模式（默认"新建"）
//
//    拖动**过程中**的修饰键决定"几何约束"：
//        Shift              → 约束为正方形 / 正圆
//        Alt（或无 Shift）  → 以起点为中心向四周扩展
//
//    ★ 为什么同一组键要分两个阶段解释 ★
//      这是 Photoshop 的既有约定，用户有肌肉记忆：想画正方形时
//      习惯"先拖起来再按 Shift"。如果让 Shift 在按下时就锁定为
//      "添加"，那"画一个正方形"就没法完成了 —— 而这两种意图
//      只能靠"什么时候按的"来区分。所以必须记下按下瞬间的状态。
//
//  ★ 依赖纪律 ★
//    本工具只 include core/Selection.h（数据结构的定义，必须完整）
//    与 tools/ 内的东西。SelectionOptions 只做前置声明 ——
//    它的定义在 ToolOptions.h 里，那个头文件拖着整个 QtWidgets。
// ============================================================================

#include "tools/AbstractTool.h"
#include "tools/ToolParams.h"   // SelectionMode 枚举（★ 零 QtWidgets 依赖 ★）
#include "core/Selection.h"

#include <QPointF>
#include <QRectF>
#include <QImage>

namespace zl {

class SelectionOptions;

class SelectionTool : public AbstractTool
{
    Q_OBJECT

public:
    enum class Kind {
        Rectangle = 0,   // 矩形选框
        Ellipse   = 1    // 椭圆选框
    };

    SelectionTool(Kind kind, Document *doc, CommandStack *stack,
                  QObject *parent = nullptr);
    ~SelectionTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;

    // ★ 快捷键的分配有点特殊 ★
    //   Photoshop 里 M 键在"矩形 / 椭圆"之间轮换，但 ToolManager 的
    //   按键查找只能返回第一个匹配项（不能表达"同一个键对应两个工具"）。
    //   解法：只给矩形选框绑 M，椭圆选框返回 0；
    //   "已经是选区工具时按 M 就切到另一种形状"这条规则由 MainWindow
    //   在 activateToolByShortcut() 里拦截实现 ——
    //   那属于"用户按键习惯"的界面策略，不该塞进工具集合的职责里。
    int shortcutKey() const override;

    ToolOptions *options() const override;

    Kind kind() const { return m_kind; }

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;
    ToolResult keyPress(const ToolEvent &e) override;

    // 拖动过程中画"正在拖出来的选区"预览（图像坐标）
    void paintOverlay(QPainter &painter, CanvasView *view) override;

    // ── 生命周期 ──
    void commit() override;
    void cancel() override;

private:
    // 由"起点 + 当前点 + 当时按住的修饰键"算出形状矩形（图像坐标，浮点）。
    //
    // ★ 为什么每次都用起点重算，而不是在上次结果上累加 ★
    //   这是本项目反复出现的同一个主题（画笔、移动工具都是如此）：
    //   累加式更新会让"先往右拖、又折回左边"这类操作产生难以预测的结果，
    //   而且 Shift 约束在方向切换时会跳变。
    //   从起点重算则永远等于"用户手上正在指的那个框"。
    QRectF shapeRect(const ToolEvent &e) const;

    // 把事件里的网格参数应用到"起点"或"当前点"上（矩形选区专用）。
    //
    // ★ 为什么吸附的是**两个角**而不是整个矩形 ★
    //   框选是"用两个角定义一个矩形"。把两个角各自吸到网格，用户就能框出
    //   "x 从 64 到 192、y 从 128 到 256"这样的整数区间 —— 这正是排版取块
    //   时想要的结果。若改成"整体平移"（像 MoveTool 那样），框的大小就
    //   锚定在用户手抖出来的尺寸上，永远不是整数格。
    //
    //   ★ 与 GridMetrics::snapOffsetForRect 的分工 ★
    //     那个函数服务"形状已定、只想把它挪到对齐位置"（移动图层）；
    //     这个函数服务"正在用两个角创造形状"（框选）。
    //     两者语义不同，不能互换。
    static QPointF snapCorner(const ToolEvent &e, const QPointF &p);

    // 把当前的预览矩形正式写入文档的选区，并压入撤销栈。
    // 松手时调用一次，以及 commit() 时补一次。
    void applySelection(const QRectF &rect);

    // 生成"矩形 / 椭圆"的填充蒙版（画布尺寸的 Grayscale8，内部 255）。
    QImage buildShapeMask(const QRectF &rect, const QSize &canvasSize) const;

    // 蒙版是否全为 0（用来识别"用户只是点了一下、没有拖动"）
    static bool isMaskEmpty(const QImage &mask);

    // ── 状态 ──

    const Kind        m_kind;
    SelectionOptions *m_options = nullptr;

    // 落笔前的选区快照。
    // ★ 必须在 mousePress 时就抓下来 ★
    //   撤销命令需要"改动前"与"改动后"两份状态。若等到松手时再抓，
    //   抓到的就已经是被本次操作污染过的状态了（尤其 Add/Subtract
    //   这类就地修改的运算）。
    Selection m_before;

    bool m_dragging = false;

    // 按下瞬间按住的修饰键所决定的运算方式。
    // 拖动期间不再改变它 —— 见头文件顶部关于"两个阶段"的说明。
    SelectionMode m_effectiveMode = SelectionMode::Replace;

    // 本次拖动是否按住了 Shift（区分"点击选择"与"拖动框选"的另一个用途：
    // 单纯点一下不拖动时，Photoshop 视为"取消选择"，不进撤销栈）
    QPointF m_startPos;      // 起点（图像坐标，浮点）
    QRectF  m_previewRect;   // 当前预览矩形（图像坐标）；空表示没有拖动
};

} // namespace zl
