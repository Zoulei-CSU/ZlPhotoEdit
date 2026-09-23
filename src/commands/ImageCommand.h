#pragma once

// ============================================================================
//  ImageCommand.h —— 整层快照类命令
// ----------------------------------------------------------------------------
//  适用于：滤镜、调整（亮度/对比度/曲线等）、去色/反色，以及**移动图层**。
//
//  ★ 关于"能不能用脏矩形优化" ★
//    原先这里写的是一句断言："这些操作会改变整层像素，无法用脏矩形优化"。
//    对滤镜/调整/去色/反色——**成立**：它们逐个像素改，改动区域就是整个图层。
//    但**移动图层不成立**：平移只是在挪位置，画布上真正变化的像素
//    只有"旧位置 ∪ 新位置"那两块。而移动恰恰是拖动时每帧都要执行一次的操作，
//    恰恰是最该省的那一类。
//
//    所以本类改成"**可选**地携带一个脏区域"（见构造函数的 dirtyRegion 参数）：
//      · 不传（默认空矩形）→ 行为与改造前**完全一致**（全图失效）；
//      · 传了            → CommandStack 只失效那一块。
//    "说得清才局部、说不清就全量"这条纪律与 DrawCommand 那边是同一条。
//
//  ★ 内存策略：qCompress 压缩 ★
//
//  4000×3000 的图层是 48MB，存前后两份就是 96MB。30 步撤销 = 2.9GB，
//  直接爆掉。但实测表明：ARGB32 的图像用 qCompress（zlib 内部实现）
//  压缩后通常只有原始的 10%~40%（取决于内容复杂度），
//  也就是说 96MB 能压到 10~40MB。
//
//  代价：每次撤销/重做要解压一次，约 30~80ms。这是可以接受的 ——
//  撤销是一个明确的一次性动作，用户按 Ctrl+Z 后等半秒是可以理解的，
//  而内存爆炸是不可接受的。
//
//  ★ 浅引用陷阱（务必牢记）★
//
//  QImage 是隐式共享的。qUncompress 返回的 QByteArray 在表达式结束后
//  会被销毁，而此时 QImage 仍然引用着它内部的缓冲区 —— 于是 QImage
//  的数据指针悬空。它**不会崩溃**（内存还没被复用），但内容会随机错乱。
//
//  这种情况最可怕的地方在于它的偶发性：
//    · 在 Debug 下可能完全正常（堆内存没被覆盖）
//    · 在 Release 下、内存压力大时才出错
//    · 出错时不是崩溃而是画面错乱，用户会以为显卡有问题
//
//  ★★ 所以：每次 qUncompress 之后，构造 QImage 时**必须**调用 copy() ★★
//
//  本文件里所有解压路径都遵守这条规则，代码中也有显式注释。
// ============================================================================

#include "commands/CommandStack.h"

#include <QUndoCommand>
#include <QByteArray>
#include <QImage>
#include <QRect>

namespace zl {

class Document;

class ImageCommand : public QUndoCommand, public IMemoryAware, public IRegionAware
{
public:
    // ── 构造（从图像数据）──
    //
    // 参数：
    //   doc   —— 目标文档
    //   index —— 目标图层索引
    //   before/after —— 改动前后的完整图层位图（会被压缩存储）
    //   text  —— 历史面板显示的文字（如"亮度/对比度"）
    //   dirtyRegion —— ★ 可选 ★ redo()/undo() 之后只有这块像素会变
    //                   （图像坐标）。不传 = 空矩形 = "我不知道" →
    //                   CommandStack 走整图失效，行为与改造前完全一致。
    //
    // 注意：构造时会立即压缩，因此传入的 QImage 可以随后释放。
    // 压缩是 CPU 密集操作，48MB 的图约 100~200ms。对于滤镜这类
    // 本来就要等几百毫秒的操作，这点额外耗时可以接受；
    // 但如果将来发现拖慢了交互，可以改成"延迟压缩"（第一次需要
    // 撤销时才压）—— 不过那样内存峰值会高一些。
    ImageCommand(Document *doc, int index,
                 const QImage &before, const QImage &after,
                 const QString &text,
                 const QRect &dirtyRegion = QRect());

    // ── 构造（从已压缩的数据）──
    //
    // 用途：从磁盘恢复撤销历史时使用（本版本未实现，接口先留）。
    // 也用于命令合并时避免"解压再压缩"的往返。
    ImageCommand(Document *doc, int index,
                 const QByteArray &compressedBefore,
                 const QByteArray &compressedAfter,
                 const QSize &imageSize,
                 const QString &text);

    void undo() override;
    void redo() override;

    // 同类的调整操作可以合并 —— 例如用户连续调整亮度滑块 10 次，
    // 只留最后一步的撤销记录。这样避免了"按 10 次撤销才能退回去"。
    int  id() const override;
    bool mergeWith(const QUndoCommand *other) override;

    qint64 memoryUsage() const override;

    // ── 区域提示（IRegionAware）──
    //
    // ★ 空矩形 = "我不知道 / 可能整张图都变了" → 调用方走全量失效 ★
    //   这正是默认值。滤镜/调整类的调用点什么都不用做就是安全的。
    //
    // ★ 使用它的唯一场景是移动图层 ★
    //   平移动的是"位置"而不是"内容"，所以变化的像素 = 旧位置 ∪ 新位置，
    //   而那两块由调用方（MoveTool）算得出来 —— 它知道位移量，
    //   也知道图层内容的包围盒。
    //
    // ★ 有一个坑：restoreImage() 里原先有一句 m_doc->markDirty() ★
    //   它跑在 push() 内部（push 会立刻执行 redo()），会把 CommandStack
    //   刚设好的区域失效**降级成全量** —— 于是"移动松手"那一下照旧卡。
    //   那句已删除，失效统一由 CommandStack 负责（见 .cpp 的说明）。
    QRect dirtyRegion() const override { return m_dirtyRegion; }

    // ── 压缩参数 ──
    //
    // 压缩级别 3 的选择依据：
    //   · 级别 1（最快）压到约 50%，省不了多少内存
    //   · 级别 9（最慢）压到约 12%，但耗时是级别 3 的 3~5 倍
    //   · 级别 3 是"压缩率已经不错，耗时还能接受"的折中点，
    //     实测 48MB 图像约 120ms
    static constexpr int kCompressionLevel = 3;

private:
    // 解压并写入图层。这是 undo/redo 的公共部分。
    void restoreImage(const QByteArray &compressed, const QSize &expectedSize);

    Document *m_doc = nullptr;
    int       m_layerIndex = 0;

    // 压缩存储的历史状态
    QByteArray m_compressedBefore;
    QByteArray m_compressedAfter;

    // 原始尺寸。用于解压失败时的校验 —— 尺寸不符说明数据损坏，
    // 应当拒绝应用而不是写入一张尺寸不对的图。
    QSize m_imageSize;

    // 合并用的命令类别 ID（每次新的调整操作序列开启一个新类别）
    int m_mergeId = 0;

    // 未压缩时的原始字节数，用于内存估算（压缩后的大小不能反映真实占用）
    qint64 m_rawBytes = 0;

    // 本次改动影响的区域（图像坐标）。空 = 不知道 → 全量失效兜底。
    QRect m_dirtyRegion;
};

} // namespace zl
