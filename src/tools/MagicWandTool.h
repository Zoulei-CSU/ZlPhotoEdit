#pragma once

// ============================================================================
//  MagicWandTool.h —— 魔术棒（按相似颜色建选区）
// ----------------------------------------------------------------------------
//  交互：在画布上**点一下**，把"与落点颜色相近且相连"的那一片变成选区。
//  不拖动、不改像素、不画叠加层 —— 它是 15 个工具里最简单的一个交互。
//
//  ★★ 它凭什么这么短：整条算法链已经在工程里了 ★★
//
//    `engine/Fill.h` 的 floodFill() 最初是为油漆桶写的，但它的返回值
//    是**一张 Grayscale8 覆盖度蒙版**，而不是"填好的图"。那个设计决定
//    （见 Fill.h 文件头）在这里兑现了第二次收益：
//
//        油漆桶 = floodFill → composeFill(蒙版, 填充色) → DrawCommand
//        魔术棒 = floodFill → applyMask(蒙版, 运算)    → SelectionCommand
//
//    前半个箭头**逐字相同**，差别只在后半段把蒙版接到哪里。于是：
//      · 容差/连通/抗锯齿的语义与油漆桶**天然一致**（同一份实现）；
//      · 选区的四种布尔运算不必重写（Selection::applyMask 本来就要支持）；
//      · 不需要任何新的 engine 代码。
//
//  ★ 决定 ①：判据取哪张图 —— 默认取**合成图** ★
//
//    油漆桶必须取活动图层（它要往那一层写像素，判据取自别处就会溢色）；
//    魔术棒改的是选区，不存在这个绑定，于是有了选择余地：
//
//      活动图层：在透明处会判成"整片同色"，用户看到的却是有内容的画面
//                —— 框出来的东西和眼睛看到的对不上；
//      合成图  ：所见即所得。与**吸管**的口径一致（吸管取合成图的理由
//                在 ColorPickerTool.h 里，是同一条）。
//
//    所以默认取合成图，并提供"只取样当前图层"开关给需要精确按层选取的场合。
//    （Photoshop 的默认恰好相反 —— 它的"Sample All Layers"默认不勾。
//      这里的取舍在选项条的提示里写清了，免得用户以为程序错了。）
//
//  ★ 决定 ②：羽化作用在"新蒙版"上，不是作用在最终选区上 ★
//
//    与 SelectionTool 完全同一个理由（见那里的注释）：先与已有选区合入
//    再羽化，会把**老选区的边界**也糊一遍，用户每做一次"添加"都让旧边界
//    更糊一层。所以顺序固定为"先羽化新蒙版 → 再按运算合入"。
//
//    ★ 与 floodFill 的抗锯齿不是一回事 ★
//      params.antialias 产生的 1~254 是**按色差**衰减的过渡带（宽度以
//      色差计），羽化是**按像素距离**的高斯模糊。两者叠加不会出错，
//      只是都让边缘更软 —— 默认羽化 0，需要时才开。
//
//  ★ 决定 ③：退化情况必须归一化 ★
//
//    "减去"或"交集"完全清空选区时，蒙版会变成一份**全 0 的活动蒙版**。
//    那种状态在语义上是"什么都没选中"，但 isActive() 返回 true，于是
//    每个工具在写像素前都要多走一遍逐像素乘蒙版的慢路径，而结果永远
//    是"什么都不写"。所以这里与 SelectionTool 一样：全空即 clear()，
//    让"没有选区 = 全选"的快路径重新生效。
//
//  ★ 决定 ④：selectAt() 必须是 public ★
//
//    理由与 BucketTool::fillAt() 一样：mousePress 只是把事件翻译成一次
//    调用，真正干活的是它。公开之后离线自检能直接驱动，不必伪造事件，
//    截图工具也能在没有鼠标的环境里造出选区。
//
//  ★ 决定 ⑤：**不检查图层是否可写**（与油漆桶的有意差异）★
//
//    油漆桶在动手前会调 canWriteToActiveLayer()，因为它是往图层里写像素的：
//    活动图层被锁定或隐藏时写进去用户也看不见，必须拦住。
//
//    魔术棒只改**选区**（文档级的元数据），不碰任何像素：
//      · 活动图层被隐藏时，用户完全可能想"先在别处框一块"；
//      · 选区不是"这一层的东西"，它是"画布上的一个范围"，
//        后续的写操作各自还会再做一次可写性检查（画笔/油漆桶都有）。
//    所以这里刻意**不**拦。若哪天有人"顺手补一句 canWriteToActiveLayer()
//    让它更一致"，那就是把两个语义不同的操作强行对齐 —— 症状是
//    "隐藏了图层之后连选区都拉不出来"，而用户会认为程序坏了。
//    测试里有一组专门钉住这条差异。
//
//  ★ 依赖纪律 ★
//    只 include tools/ 与 core/Selection.h、engine/Fill.h 这类**纯数据/纯函数**
//    头文件。不 include ui/ 下的任何东西（分层铁律），也不持有控件指针
//    （参数走 MagicWandOptions 的信号）。
// ============================================================================

#include "tools/AbstractTool.h"
#include "tools/ToolParams.h"
#include "core/Selection.h"

#include <QPoint>
#include <QImage>

namespace zl {

class MagicWandOptions;

class MagicWandTool : public AbstractTool
{
    Q_OBJECT

public:
    MagicWandTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~MagicWandTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;
    // ★ W —— 对齐 Photoshop 的魔术棒 ★
    //   本工程不做"快速选择 / 对象选择"，所以 W 不会被轮换逻辑分走，
    //   直接在 ToolManager::findByShortcut() 命中本工具。
    int shortcutKey() const override;

    ToolOptions *options() const override;

    // ── 输入事件 ──
    // 只有 mousePress：魔术棒是"点一下"的工具，没有拖动、没有预览。
    ToolResult mousePress(const ToolEvent &e) override;

    // ── ★ 干活的那半（见头文件顶部决定 ④）★ ──
    //
    // 在 imagePos 处取样并按当前参数建立/更新选区。返回是否产生了改动
    // （false 包括：未就绪、无撤销栈、点在画布外、区域为空、结果与
    //  改动前语义相同 —— 这些情况都**不压撤销**）。
    //
    // modifiers 用于 Photoshop 那套临时运算键：
    //   Shift → 添加、Alt → 减去、Shift+Alt → 交集、无 → 选项条上的模式。
    //
    // ★ 为什么把修饰键做成参数而不是让函数自己去读 ★
    //   工具的事件入口才有修饰键，而自检要能一条一条地驱动四种运算。
    //   让函数去读某个隐藏状态的话，测试就只能伪造事件了。
    bool selectAt(const QPoint &imagePos, ToolModifiers modifiers = ToolModifiers());

private:
    MagicWandOptions *m_options = nullptr;
};

} // namespace zl
