// ============================================================================
//  ToolOptionsBar.cpp —— 工具选项条的实现
// ----------------------------------------------------------------------------
//  本文件很短，但有两个必须写对的地方：
//
//    ★ 1. 切换工具时必须"彻底"清掉旧内容 ★
//
//      只在布局里 removeWidget 是不够的 —— 控件仍然挂在父控件上，
//      它的信号也仍然连着。结果是：
//        · 界面上看到旧控件还残留在原位（重叠在新控件上）
//        · 改新工具的参数会触发旧工具的参数对象也跟着变
//      正确做法是"三件套"：
//        takeAt → setParent(nullptr) → deleteLater
//      其中 setParent(nullptr) 是让控件立刻从视觉树里消失（立即生效），
//      而 deleteLater 是让它安全析构（不能在"可能正在处理它的信号"的
//      调用栈里直接 delete）。
//
//    ★ 2. 不要缓存控件指针 ★
//
//      本类只在 setTool() 期间短暂持有控件，之后不再引用。
//      工具的参数对象通过信号暴露变化，而不通过控件 ——
//      这样控件可以随时被销毁，工具层永远不会拿到野指针。
// ============================================================================

#include "ui/ToolOptionsBar.h"
#include "tools/AbstractTool.h"
#include "tools/ToolOptions.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QFont>
#include <QWidget>

namespace zl {

ToolOptionsBar::ToolOptionsBar(QWidget *parent)
    : QWidget(parent)
{
    // ★ 用 QHBoxLayout 而不是 QFormLayout ★
    //   选项条是**横向**的一条，所有控件排成一行。
    //   QFormLayout 是"标签 + 控件"两列纵向排列，那是对话框的布局。
    m_layout = new QHBoxLayout(this);

    // 边距压到很小 —— 选项条本身很窄（约 30px 高），
    // 默认的 9px 边距会让它看起来空空荡荡。
    m_layout->setContentsMargins(6, 2, 6, 2);
    m_layout->setSpacing(8);

    // 初始状态：没有工具
    setTool(nullptr);
}

ToolOptionsBar::~ToolOptionsBar() = default;

// ============================================================================
//  setTool —— 切换当前工具并重建选项条
// ============================================================================

void ToolOptionsBar::setTool(AbstractTool *tool)
{
    // ── 同一个工具重复调用 → 什么都不做 ──
    //
    // ★ 这个提前返回很重要 ★
    //   ToolManager::setActiveTool 会在"已经是这个工具"时也发信号
    //   （有些实现会，有些不会 —— 不能依赖）。如果不做这个检查，
    //   每次点击同一个工具按钮都会销毁并重建整条选项条：
    //     · 用户正在输入框里打字时点击工具按钮 → 输入内容和焦点丢失
    //     · 白白产生一轮控件的 new/delete
    if (tool == m_currentTool)
        return;

    m_currentTool = tool;

    // ── 清空旧内容 ──
    //
    // ★ 必须是"倒着取"（或者每次都取第 0 项）★
    //   takeAt(0) 会移除第一项，之后的项索引整体前移，
    //   所以循环里始终取 0 就是正确的遍历方式。
    //   正序遍历（i = 0..count）会因为索引前移而漏掉一半的项 ——
    //   表现为"切换工具后界面上残留了上一次的一半控件"。
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            // ★ 顺序：先脱离父控件，再延迟删除 ★
            //   setParent(nullptr) 让它立刻从界面上消失；
            //   如果只 deleteLater 而不 setParent，
            //   在当前事件循环返回之前它仍然可见并可能盖住新控件。
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;   // QLayoutItem 本身必须手动删除（它不是 QObject）
    }

    // ── 没有工具 → 显示一句提示 ──
    //
    // ★ 为什么不留空 ★
    //   留空的话那条工具栏会缩到 0 高度，主工具栏与画布之间的间距
    //   会突然变化 —— 视觉上像是"界面抖了一下"。
    //   放一句固定高度的提示文字可以保持高度稳定。
    if (!tool) {
        auto *label = new QLabel(tr("没有活动的工具"), this);
        label->setEnabled(false);   // 灰掉，表示这不是"可用内容"
        m_layout->addWidget(label);
        m_layout->addStretch(1);
        return;
    }

    // ── 借工具的参数对象构建控件 ──
    //
    // ★ 注意：控件由 ToolOptions::createWidget 创建，
    //   所有权交给调用方（也就是这里的 this）★
    //   所以这里要把 parent 传成 this ——
    //   这样即使某天上面的清理逻辑出问题，Qt 的对象树也能兜底回收。
    ToolOptions *options = tool->options();

    if (!options) {
        // 工具没有参数对象（例如"缩放工具"可能不需要参数）→ 只显示名字
        auto *label = new QLabel(tool->displayName(), this);
        m_layout->addWidget(label);
        m_layout->addStretch(1);
        return;
    }

    QWidget *content = options->createWidget(this);

    if (!content) {
        // 参数对象存在但选择不提供界面（合法情况）
        auto *label = new QLabel(tool->displayName(), this);
        m_layout->addWidget(label);
        m_layout->addStretch(1);
        return;
    }

    // ── 给控件加上工具名标签 ──
    //
    // 用户从"画笔"切到"橡皮"时，两者的选项看起来几乎一样
    // （都是大小/硬度/不透明度）。加一个名字标签能明确当前是哪个工具，
    // 避免"我明明调了画笔大小，怎么画出来的是橡皮"这类困惑。
    auto *nameLabel = new QLabel(tool->displayName(), this);
    QFont f = nameLabel->font();
    f.setBold(true);
    nameLabel->setFont(f);
    m_layout->addWidget(nameLabel);

    // 分隔线：用一条竖线把工具名与参数分开
    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    m_layout->addWidget(separator);

    m_layout->addWidget(content);

    // ★ 末尾加弹性空间 ★
    //   不加的话所有控件会被拉伸铺满整条宽度 ——
    //   一个 QSpinBox 变成 200 像素宽会很难看。
    m_layout->addStretch(1);
}

} // namespace zl
