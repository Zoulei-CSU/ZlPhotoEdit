#include "commands/SelectionCommand.h"
#include "commands/ImageCommand.h"   // ★ 复用它的 kCompressionLevel 常量 ★
#include "core/Document.h"

namespace zl {

// ===========================================================================
//  构造
// ===========================================================================

SelectionCommand::SelectionCommand(Document *doc,
                                   const Selection &before,
                                   const Selection &after,
                                   const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_before(pack(before))
    , m_after(pack(after))
{
}

// ===========================================================================
//  打包 / 还原
// ===========================================================================

SelectionCommand::PackedSelection SelectionCommand::pack(const Selection &sel)
{
    PackedSelection packed;

    packed.active      = sel.isActive();
    packed.allSelected = sel.isAllSelected();

    // 全选状态下没有蒙版，不需要存数据。
    // 这是一个很实在的优化：用户的绝大多数操作都在"无选区"状态下进行，
    // 此时存储开销为零。
    if (!sel.isActive() || sel.isAllSelected() || sel.mask().isNull())
        return packed;

    const QImage &mask = sel.mask();
    packed.maskSize = mask.size();

    // ── 压缩蒙版 ──
    //
    // 压缩级别用 3（与 ImageCommand 一致）。
    // 蒙版的压缩效果特别好：典型选区是"大片 0 + 一块 255"，
    // 高度规律的数据用 zlib 能压到原始大小的 1% 甚至更低。
    // 一张 12MB 的蒙版可能只剩 100KB。
    packed.compressedMask = qCompress(
        mask.constBits(),
        int(mask.sizeInBytes()),
        ImageCommand::kCompressionLevel);

    return packed;
}

void SelectionCommand::applyPacked(const PackedSelection &packed)
{
    if (!m_doc)
        return;

    Selection &sel = m_doc->selection();

    // ── 情况 1：全选（无选区）──
    if (!packed.active || packed.allSelected) {
        sel.clear();
        m_doc->markDirty();
        return;
    }

    // ── 情况 2：有蒙版 ──
    if (packed.compressedMask.isEmpty() || !packed.maskSize.isValid()) {
        // 数据不完整（可能是压缩失败），保守地清空选区而不是留一个残留状态
        sel.clear();
        m_doc->markDirty();
        return;
    }

    const QByteArray raw = qUncompress(packed.compressedMask);
    if (raw.isEmpty()) {
        // 解压失败：与 ImageCommand 同样的降级策略 ——
        // 放弃这一步而不是崩溃。用户宁可选区没恢复，也不想程序挂掉。
        return;
    }

    const int width  = packed.maskSize.width();
    const int height = packed.maskSize.height();

    // 尺寸校验
    const qint64 expectedBytes = qint64(width) * qint64(height);
    if (qint64(raw.size()) < expectedBytes)
        return;

    // ★ 必须 copy() ★
    // 与 ImageCommand::restoreImage 里同样的原因：qUncompress 返回的是
    // 局部 QByteArray，函数返回后销毁。不 copy 的话 QImage 会引用一块
    // 已释放的内存 —— 不崩溃，但数据随机错乱。这是本项目最隐蔽的陷阱，
    // 每一处解压路径都要注意。
    const QImage mask =
        QImage(reinterpret_cast<const uchar *>(raw.constData()),
               width, height, width,
               QImage::Format_Grayscale8).copy();

    if (mask.isNull())
        return;

    sel.setMask(mask);
    m_doc->markDirty();
}

// ===========================================================================
//  undo / redo
// ===========================================================================

void SelectionCommand::redo()
{
    applyPacked(m_after);
}

void SelectionCommand::undo()
{
    applyPacked(m_before);
}

// ===========================================================================
//  内存报告
// ===========================================================================

qint64 SelectionCommand::memoryUsage() const
{
    return qint64(m_before.compressedMask.size())
         + qint64(m_after.compressedMask.size())
         + qint64(sizeof(SelectionCommand));
}

} // namespace zl
