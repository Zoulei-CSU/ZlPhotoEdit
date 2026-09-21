#pragma once

// ============================================================================
//  TextTool.h —— 文字工具
// ----------------------------------------------------------------------------
//  ★★ 它是 14 个工具里唯一"有第二份界面"的一个 ★★
//
//  别的工具的全部交互都在画布的鼠标事件里；文字工具多了一个**输入浮层**
//  （一个浮在画布上的 QTextEdit），因为 QPainter::drawText 只能"画"，
//  没法输入中文、没法编辑光标、没法退格。
//
//  ★★ 于是职责被切成两半，切法只有一种是对的 ★★
//
//      内容（用户敲了什么）→ **工具**（它是模型）
//      浮层（QTextEdit）    → **画布**（它是界面）
//
//  为什么浮层不能归工具：QTextEdit 必须挂在画布上才能定位到"点击处"，
//  而 tools 层不允许依赖 ui 层（这是分层的铁律，也是 run_tests.sh 里
//  "工具自检刻意不链 ui/" 守着的那条边界）。所以浮层只能由画布创建。
//
//  两端怎么牵线：工具发 requestTextEditor()（实参只有内容：锚点、字体、
//  颜色、对齐），画布开浮层；用户敲字时画布把文本**推**回来
//  （setEditingText，与 setForegroundColor 同一个套路）；用户确认时画布
//  调 commit()，工具落像素压栈。**工具全程不认识 QTextEdit。**
//
//  ★★ 为什么不做"文字图层 + 重新编辑" ★★
//
//  本工程没有文字图层（见「明确不做的事」），文字一次落进像素层，
//  之后就是普通像素 —— 不存在"双击再改字"这一步，也不存在栅格化。
//  这与产品已定案的"不做 .zlpe、只导出扁平图"是同一条取舍：
//  要可再编辑就必须有第二条数据通路（矢量文字），那是另一个工程。
//
//  ★★ 一次输入 = 一条撤销记录 ★★
//
//  浮层里敲多少字、换多少行都不产生命令；只有 commit() 那一下压一条
//  DrawCommand::standalone()。理由与油漆桶/形状相同：让撤销栈里出现
//  "逐字符撤销"会毁掉 Ctrl+Z 的语义（用户期望撤掉整段文字）。
//
//  ★ 依赖纪律 ★
//    只 include tools/ 、core/ 、engine/ 的头，不 include 任何 ui/ 的头。
//    TextOptions 只做前置声明 —— 它的定义在 ToolOptions.h 里，
//    那个头文件拖着整个 QtWidgets。
// ============================================================================

#include "tools/AbstractTool.h"
#include "tools/ToolParams.h"
#include "engine/Text.h"

#include <QImage>
#include <QRect>
#include <QPointF>
#include <QColor>
#include <QFont>
#include <QString>

namespace zl {

// TextOptions 的定义在 ToolOptions.h（拖着整个 QtWidgets），这里只前置声明。
class TextOptions;

class TextTool : public AbstractTool
{
    Q_OBJECT

public:
    TextTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~TextTool() override;

    // ── 身份 ──
    QString id() const override;                 // "text"
    QString displayName() const override;        // 文字
    int     shortcutKey() const override;        // T
    QCursor cursor() const override;             // 文本工字形

    ToolOptions *options() const override;

    // ── 颜色同步 ──
    //   前景色 → 文字颜色。理由：文字是"用前景色画出来的东西"，
    //   与画笔、直线一致；工具箱底部那对色块里，用户盯着的是前景色。
    void setForegroundColor(const QColor &color) override;

    // ── 输入事件 ──
    //
    //  单击：在点击处开一个输入浮层。
    //    · 已经有一个浮层开着 → 先把它提交（与 Photoshop 一致：
    //      点别处 = 这段文字写完了，同时开始下一段）；
    //    · 不给浮层的文本内容，内容由画布推回来（见 setEditingText）。
    ToolResult mousePress(const ToolEvent &e) override;

    // ── 内嵌编辑框协议（见 AbstractTool.h）──
    void setEditingText(const QString &text) override;
    bool hasInlineEditor() const override;

    // ── 生命周期 ──
    void commit() override;      // 把正在编辑的文本落到图层并压一条命令
    void cancel() override;      // Esc：丢弃这段文字，撤销栈里不留痕
    bool isBusy() const override;

    // 换文档时把浮层状态清干净（基类会取消正在进行的操作）。
    void setDocument(Document *doc) override;

    // ── 自检 / 截图入口 ──
    //
    //  ★ 为什么不走 mousePress ★
    //    mousePress 会发 requestTextEditor，而自检与截图工具**没有浮层**，
    //    截图工具更不该因为落一段字就在画面上多出一个输入框。
    //    这个入口是"跳过浮层、直接把字落到图层上"，等价于
    //    "开浮层 → 输入 → 确认"三步走完的最终效果。
    //
    //  返回是否真的产生了像素改动（空文本 / 画布外 / 图层不可写都返回 false）。
    bool placeText(const QString &text, const QPointF &anchor);

    // ── 观测口（自检用）──
    bool    isEditing() const { return m_editing; }
    QString editingText() const { return m_text; }
    QPointF editAnchor() const { return m_anchor; }

    // 当前应当使用的字体（像素字号已按文档 DPI 算好；字距已写进字体）。
    // 画布开浮层时用它，自检也用它 —— 这样"工具算的字体"只有一个来源。
    QFont   editorFont() const;

private:
    // 开始一次编辑。notifyUi 为 false 时不发 requestTextEditor（见 placeText）。
    bool beginEditAt(const QPointF &imagePos, bool notifyUi);

    // 真正把文字落到活动图层并压一条撤销命令。
    bool renderText(const QString &text, const QPointF &anchor);

    // 收尾：清掉"正在编辑"的全部状态（不碰像素、不碰撤销栈）。
    void clearEditState();

    // ── 状态 ──
    TextOptions *m_options = nullptr;

    // 是否有一个编辑会话在进行。
    //
    // ★ 为什么不用基类的 m_busy ★
    //   基类的 isBusy() 是"是否有正在进行的操作"，本工具重写了 isBusy()
    //   直接返回 m_editing —— 两者语义相同，但**浮层的开/关**必须由一个
    //   本类自己的标志驱动：画布在 Esc 与"换工具"两条路径上都会问
    //   hasInlineEditor()，那必须严格反映"浮层还在不在"。
    bool    m_editing = false;

    // 锚点（图像坐标）。★ 全程不变 ★ —— 用户点了哪里，文字就从哪里开始排。
    QPointF m_anchor;

    // 当前浮层里的文本（由画布在每次 textChanged 时推过来）。
    QString m_text;
};

} // namespace zl
