#pragma once

// ============================================================================
//  DrawCommand.h —— 局部像素修改的撤销命令（脏矩形方案）
// ----------------------------------------------------------------------------
//  ★ 核心设计：只存改动区域，不存整个图层 ★
//
//  画笔是最高频的操作，一次描边可能产生几十上百个中间状态。
//  如果每个状态都存整层快照，4000×3000 的图层每步 48MB，
//  30 步撤销就是 1.4GB —— 内存直接爆炸。
//
//  实际情况是：画一笔只影响画布上很小的一块区域。所以只存
//  "改动前的旧像素" + "改动后的新像素"，且只存这块区域的矩形。
//  一条 100 像素长的粗画笔描边，脏矩形大约 200×50，约 40KB ——
//  比整层快照小 1200 倍。
//
//  ★ 必须预留 layerIndex ★
//
//  从第一天起就带上 layerIndex 字段。现在（单图层阶段）它总是 0，
//  但等到多图层阶段，如果命令里没有这个字段，所有工具的构造调用点
//  都要改一遍。这是典型的"现在加一行，将来省一天"。
//
//  ★ 坐标约定 ★
//
//  所有坐标都是**图像坐标**（画布坐标系），不是控件坐标。
//  工具负责把鼠标位置换算好再传进来 —— 因为图像坐标在缩放/平移
//  变化时保持不变，控件坐标会变。撤销栈里存的东西必须不随时间变化。
// ============================================================================

#include "commands/CommandStack.h"

#include <QUndoCommand>
#include <QImage>
#include <QRect>
#include <QString>
#include <QList>

namespace zl {

class Document;

class DrawCommand : public QUndoCommand, public IMemoryAware, public IRegionAware
{
public:
    // ── 区域提示：让 CommandStack 只失效真正改动的矩形（IRegionAware）──
    //
    // 本命令改动的东西就是"layerIndex 这一层上 m_rect 这一块"，
    // redo() 写 m_after、undo() 写 m_before，位置完全相同 ——
    // 所以这个问题可以**精确回答**，不需要估。
    //
    // ★ 为什么值得实现在这里 ★
    //   DrawCommand 是画笔 / 橡皮 / 油漆桶 / 形状 / 文字共用的命令类，
    //   也就是最高频的那一类。改造前 CommandStack 每次压栈都无条件
    //   全图失效 → 每画完一笔松手都要整张画布重算一次
    //   （实测 4K 431 ms、8K 4134 ms 的停顿），
    //   而那一笔真正改动的往往只有几十 KB。
    QRect dirtyRegion() const override { return m_rect; }

    // ── 构造 ──
    //
    // 参数：
    //   doc        —— 目标文档（不持有所有权）
    //   layerIndex —— 目标图层索引（★ 预留字段，见头文件说明）
    //   rect       —— 脏矩形（图像坐标）
    //   strokeId   —— 描边编号。★ 必须由调用方显式给出，理由见下 ★
    //   before     —— 改动前的像素（尺寸应等于 rect 的尺寸）
    //   after      —— 改动后的像素（尺寸应等于 rect 的尺寸）
    //   text       —— 显示在历史面板里的描述，如"画笔描边"
    //
    // 关于 before/after 的格式：都必须是 ARGB32_Premultiplied，
    // 且必须是对应区域的**独立副本**（不是原图的子图共享内存）。
    // 调用方用 QImage::copy(rect) 来保证这一点。
    //
    // ★★ 为什么 strokeId 要显式传，而不是构造函数自己去取一个 ★★
    //
    //   这里原先写的是 `m_strokeId = currentStrokeId()`，也就是"取最近一次
    //   分配出去的编号"。它的前提是"工具在 mousePress 时调用过 beginStroke()"——
    //   而**全工程没有任何地方调用过它**（实测 grep 确认）。
    //   于是每一条 DrawCommand 拿到的编号都是同一个常量 0，
    //   于是 id() 全部相同，于是 QUndoStack 认为"这些是同一次描边"，
    //   调用 mergeWith() 把它们合并成一条。
    //
    //   症状（实测）：连点两下油漆桶，撤销栈里只有一条记录 ——
    //   Ctrl+Z 一次把两处改动一起撤掉。画笔也一样：画两笔算一步。
    //   而**编译期、运行期、其余自检全都没有任何提示**。
    //
    //   现在改成显式传参：忘写的代价从"静默合并"变成"编译不过"。
    //   绝大多数调用点要的是"这条命令自成一档"，直接调 standalone() 即可；
    //   只有当一次拖动确实要压多条命令、且希望它们合并成一步时，
    //   才自己 new 并传同一个 strokeId。
    DrawCommand(Document *doc, int layerIndex, const QRect &rect, int strokeId,
                const QImage &before, const QImage &after,
                const QString &text);

    // ── 独立操作：一次点击 / 一笔 = 一条撤销记录 ──
    //
    // 内部自动取一个**新的**描边编号，因此绝不会与相邻命令撞号。
    // 油漆桶的"点一下填一块"、画笔的"松手压整笔"都用它。
    //
    // 返回值用裸指针（而不是 unique_ptr）：它的所有权马上要交给
    // CommandStack::push()，包一层反而要在调用处 .release()。
    static DrawCommand *standalone(Document *doc, int layerIndex, const QRect &rect,
                                   const QImage &before, const QImage &after,
                                   const QString &text);

    // ── QUndoCommand 接口 ──
    void undo() override;
    void redo() override;

    // ── 合并优化 ──
    //
    // ★ 这是本命令类最重要的优化 ★
    //
    // 一次鼠标拖动会产生大量"落笔点"事件（每秒可达上百个）。
    // 如果每个点都产生一个独立的撤销步骤，用户画一笔要按 100 次撤销
    // 才能退回 —— 完全不可用。
    //
    // QUndoStack 提供了 mergeWith() 机制：当新命令的 id() 与栈顶命令相同时，
    // 会调用栈顶命令的 mergeWith(newCmd) 尝试合并。
    //
    // 画笔场景的合并规则：
    //   · 同一次描边内（same stroke）的命令可以合并
    //   · 合并后的脏矩形 = 两个矩形的并集
    //   · 合并后的 before = 原命令的 before（最早的旧状态）
    //                      改写为"取并集后重新截取"是更正确的做法
    //   · 合并后的 after  = 新命令的 after（最新的新状态）
    //
    // 判断"是否同一次描边"由 id() 承担：同一次描边返回相同 id，
    // 不同描边返回不同 id。用一个自增的描边计数器实现。
    int id() const override;
    bool mergeWith(const QUndoCommand *other) override;

    // ── 内存报告 ──
    qint64 memoryUsage() const override;

    // ── 访问器（供调试与测试）──
    QRect dirtyRect() const { return m_rect; }
    int   layerIndex() const { return m_layerIndex; }

    // ── 描边编号的分配 ──
    //
    // 想让"一次按住拖动的全部中间状态"合并成一步撤销时，用法是：
    //   mousePress   → int sid = DrawCommand::beginStroke();
    //   每个 move    → new DrawCommand(..., sid, ...)   ← 全部传同一个 sid
    //   mouseRelease → 什么都不用做（下一次 press 会自然开启新描边）
    //
    // ★ currentStrokeId() 已删除 ★
    //   它返回"最近一次分配出去的编号"，看起来像是"当前描边号"，
    //   实际在没人调 beginStroke() 时它只是一个不变的常量 0 ——
    //   谁用它，谁的所有命令就会被合并成一条（见构造函数的注释）。
    //   留着一个能"安静地用错"的接口比没有接口更危险，所以删掉。
    //   要编号就自己 beginStroke() 取。
    static int beginStroke();

private:
    // 把指定矩形的内容写入文档的指定图层
    void applyImage(const QImage &patch);

    Document *m_doc = nullptr;

    int   m_layerIndex = 0;
    QRect m_rect;

    QImage m_before;    // 改动前的像素
    QImage m_after;     // 改动后的像素

    int m_strokeId = 0; // 描边序号，用于 mergeWith 的判断
};

} // namespace zl
