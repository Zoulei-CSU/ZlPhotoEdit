#pragma once

// ============================================================================
//  SelectionCommand.h —— 选区变更的撤销命令
// ----------------------------------------------------------------------------
//  选区蒙版是 Grayscale8（每像素 1 字节），4000×3000 约 12MB。
//  同样用 qCompress 压缩存储 —— 蒙版通常是"大片 0 + 一块 255"的结构，
//  压缩率极高（常能压到 1% 以下，因为重复字节非常多）。
//
//  与 ImageCommand 的区别：
//    ImageCommand    —— 存 ARGB32 的图层位图（每像素 4 字节）
//    SelectionCommand —— 存 Grayscale8 的蒙版（每像素 1 字节）+
//                        一个"是否处于全选状态"的标志
//
//  ★ 全选状态的表示 ★
//
//  Selection 有两种状态：
//    a) isActive() == false —— 没有选区，等价于"全选"
//    b) isActive() == true  —— 有一份蒙版
//
//  撤销时必须能在这两种状态间正确切换。例如：
//    用户选区 → 按 Ctrl+D 取消选择 → 撤销 → 应该回到"有选区"
//  如果只存蒙版，就无法表达"取消选择"这个状态（那时蒙版是空的）。
//  所以必须把 m_active/m_allSelected 一并保存。
// ============================================================================

#include "commands/CommandStack.h"
#include "core/Selection.h"

#include <QUndoCommand>
#include <QByteArray>

namespace zl {

class Document;

class SelectionCommand : public QUndoCommand, public IMemoryAware
{
public:
    // ── 构造 ──
    //
    // 参数为改动前后的 Selection 快照（由调用方在操作前后各取一次）。
    // 命令内部会压缩存储。
    SelectionCommand(Document *doc,
                     const Selection &before,
                     const Selection &after,
                     const QString &text);

    void undo() override;
    void redo() override;

    qint64 memoryUsage() const override;

private:
    // 打包一个 Selection 为可压缩的数据
    struct PackedSelection
    {
        QByteArray compressedMask;
        QSize      maskSize;
        bool       active      = false;
        bool       allSelected = true;

        bool isValid() const { return active || allSelected; }
    };

    static PackedSelection pack(const Selection &sel);
    void applyPacked(const PackedSelection &packed);

    Document *m_doc = nullptr;

    PackedSelection m_before;
    PackedSelection m_after;
};

} // namespace zl
