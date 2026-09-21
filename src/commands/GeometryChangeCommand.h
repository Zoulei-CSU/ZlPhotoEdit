#pragma once

// ============================================================================
//  GeometryChangeCommand.h —— 尺寸 / 旋转 / 翻转 / 裁剪的统一撤销命令
// ----------------------------------------------------------------------------
//  设计要点：**这四类操作本质相同** —— 都是"把所有图层换成另一组位图，
//  把画布换成一个新尺寸，把选区换成一份新蒙版"。
//
//  所以只用一个 Command 类，而不是 ResizeCommand / RotateCommand /
//  CropCommand / FlipCommand 四个类。收益：
//    · 撤销逻辑只写一遍（这是最容易出错、最需要测试的部分）
//    · 内存策略只实现一次（qCompress + 解压后 copy 的修正）
//    · 新增几何操作（例如将来的透视变形）不需要新写一个 Command
//
//  ★ 这是最耗内存的一类命令 ★
//
//  4 个图层 × 4000×3000 × 4 字节 × 2 份（前/后）= 384MB 一步。
//
//  内存控制的三道措施：
//    1. qCompress 级别 3 —— 实测能压到 15%~40%
//    2. 单独限制这类命令的步数（在 CommandStack 层面按类型统计）
//    3. 若压缩后仍然过大，可以选择不记录这一步（降级）
//
//  ★ 浅引用陷阱在这个文件里出现了两次 ★
//    一次是图层位图的解压，一次是选区蒙版的解压。两处都必须 copy()。
// ============================================================================

#include "commands/CommandStack.h"
#include "core/Layer.h"
#include "core/Selection.h"

#include <QUndoCommand>
#include <QList>
#include <QSize>
#include <QRect>

namespace zl {

class Document;

class GeometryChangeCommand : public QUndoCommand, public IMemoryAware
{
public:
    // ── 操作类型 ──
    // 只用于生成历史面板的显示文字与内存统计，不影响执行逻辑。
    enum class Kind {
        ScaleImage,        // 图像大小
        ResizeCanvas,      // 画布大小
        Rotate90,          // 90° 倍数旋转
        RotateArbitrary,   // 任意角度旋转
        Flip,              // 翻转
        Crop               // 裁剪
    };

    // ── 构造 ──
    //
    // 参数为改动前后的完整状态快照。调用方（通常是 MainWindow 或对话框）
    // 在操作前调用 captureState() 拿一份 "before"，操作后拿一份 "after"。
    //
    // 用快照而不是"操作参数"（如 newSize + anchor）的原因：
    //   几何操作的边界情况非常多（内容超出画布、选区超出、旋转后尺寸计算
    //   的舍入等），用参数重放一遍很容易与第一次执行的结果不完全一致。
    //   存快照则是"所见即所得"，撤销时一定是回到那个确切的状态。
    //   代价是内存，但这个代价可以用压缩控制。
    GeometryChangeCommand(Document *doc,
                          const QList<Layer> &layersBefore,
                          const QList<Layer> &layersAfter,
                          const QSize &sizeBefore,
                          const QSize &sizeAfter,
                          const Selection &selectionBefore,
                          const Selection &selectionAfter,
                          int activeBefore,
                          int activeAfter,
                          Kind kind,
                          const QString &text);

    void undo() override;
    void redo() override;

    qint64 memoryUsage() const override;

    // 这类命令是否值得记录进撤销栈。
    //
    //  由调用方在 push 之前查询：如果返回 false，说明存储代价过高
    //  （压缩后仍然超过阈值），可以选择"执行操作但不记录撤销"，
    //  并提示用户。这比让程序因为内存耗尽而崩溃要好得多。
    bool isWorthRecording(qint64 maxBytes = 256LL * 1024 * 1024) const;

    // ── 状态快照辅助 ──
    //
    // 提供一个轻量的快照结构，方便调用方"先存 before、操作、再取 after"。
    struct Snapshot
    {
        QList<Layer> layers;
        QSize        canvasSize;
        Selection    selection;
        int          activeIndex = 0;
    };

    // 从文档抓取当前状态。
    // 注意：这里的 Layer 是浅拷贝（QImage 共享），所以快照本身很轻。
    // 真正的内存开销在 GeometryChangeCommand 构造时的压缩。
    static Snapshot captureState(const Document *doc);

private:
    // 一个图层状态集的压缩表示
    struct PackedLayers
    {
        QList<QByteArray> compressedImages;   // 每个图层一个压缩块
        QList<Layer>      metadata;           // 图层元数据（名称/属性，不压缩）
        QSize             canvasSize;
        int               activeIndex = 0;

        qint64 compressedBytes() const
        {
            qint64 total = 0;
            for (const QByteArray &ba : compressedImages)
                total += qint64(ba.size());
            return total;
        }
    };

    struct PackedSelection
    {
        QByteArray compressedMask;
        QSize      maskSize;
        bool       active      = false;
        bool       allSelected = true;
    };

    static PackedLayers    packLayers(const QList<Layer> &layers,
                                      const QSize &canvasSize,
                                      int activeIndex);
    static PackedSelection packSelection(const Selection &sel);

    // 把打包的状态应用回文档
    void applyPackedLayers(const PackedLayers &packed);
    void applyPackedSelection(const PackedSelection &packed);

    Document *m_doc = nullptr;

    PackedLayers    m_beforeLayers;
    PackedLayers    m_afterLayers;
    PackedSelection m_beforeSelection;
    PackedSelection m_afterSelection;

    Kind m_kind;
};

} // namespace zl
