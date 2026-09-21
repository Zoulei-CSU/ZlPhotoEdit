#include "commands/GeometryChangeCommand.h"
#include "commands/ImageCommand.h"
#include "core/Document.h"

#include <QPainter>

namespace zl {

// ===========================================================================
//  构造
// ===========================================================================

GeometryChangeCommand::GeometryChangeCommand(
        Document *doc,
        const QList<Layer> &layersBefore,
        const QList<Layer> &layersAfter,
        const QSize &sizeBefore,
        const QSize &sizeAfter,
        const Selection &selectionBefore,
        const Selection &selectionAfter,
        int activeBefore,
        int activeAfter,
        Kind kind,
        const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_kind(kind)
{
    // ★ 内存开销集中在这一步 ★
    //
    // 这里有 2（前/后）× N（图层数）次压缩，每次对 4000×3000 的图
    // 约需 100~200ms。4 个图层就是 8 次，总计约 1 秒。
    //
    // 这意味着"执行一个图像大小调整"这个动作，用户会感到约 1 秒的延迟。
    // 这是当前设计的已知代价，接受它的理由是：内存是硬约束，而
    // 1 秒的等待在"改变图像尺寸"这种低频重操作上是可接受的。
    //
    // 如果将来觉得太慢，优化方向有两条：
    //   1. 延迟压缩（构造时只存 QImage 引用，第一次需要撤时才压）——
    //      但那样内存峰值会很高，与设计目标矛盾
    //   2. 改用多线程并行压缩 —— QtConcurrent 可用，能压到约 250ms
    //      这是更合理的优化方向，Phase 7 的健壮性阶段可以考虑
    m_beforeLayers = packLayers(layersBefore, sizeBefore, activeBefore);
    m_afterLayers  = packLayers(layersAfter,  sizeAfter,  activeAfter);

    m_beforeSelection = packSelection(selectionBefore);
    m_afterSelection  = packSelection(selectionAfter);
}

// ===========================================================================
//  打包
// ===========================================================================

GeometryChangeCommand::PackedLayers
GeometryChangeCommand::packLayers(const QList<Layer> &layers,
                                  const QSize &canvasSize,
                                  int activeIndex)
{
    PackedLayers packed;
    packed.canvasSize  = canvasSize;
    packed.activeIndex = activeIndex;

    packed.metadata.reserve(layers.size());
    packed.compressedImages.reserve(layers.size());

    for (const Layer &layer : layers) {
        // 元数据部分直接存 Layer 对象 —— 它内部只持有 QImage 的
        // 隐式共享引用，不会真的拷贝像素。但我们随后会压缩像素，
        // 所以这里必须把 QImage 清掉，避免"压缩了还留着一份引用"
        // 导致像素内存被两个地方同时占着。
        Layer meta = layer;
        const QImage image = layer.image();

        // 清空元数据对象里的位图。
        // 这样 packed.metadata 里存的是纯元数据（几十字节），
        // 像素数据只存在于压缩后的字节流里。
        meta.setImage(QImage());

        packed.metadata.append(meta);

        // 压缩像素
        if (image.isNull()) {
            packed.compressedImages.append(QByteArray());
            continue;
        }

        packed.compressedImages.append(
            qCompress(image.constBits(),
                      int(image.sizeInBytes()),
                      ImageCommand::kCompressionLevel));
    }

    return packed;
}

GeometryChangeCommand::PackedSelection
GeometryChangeCommand::packSelection(const Selection &sel)
{
    PackedSelection packed;
    packed.active      = sel.isActive();
    packed.allSelected = sel.isAllSelected();

    if (!sel.isActive() || sel.isAllSelected() || sel.mask().isNull())
        return packed;

    const QImage &mask = sel.mask();
    packed.maskSize = mask.size();

    packed.compressedMask = qCompress(
        mask.constBits(),
        int(mask.sizeInBytes()),
        ImageCommand::kCompressionLevel);

    return packed;
}

// ===========================================================================
//  undo / redo
// ===========================================================================

void GeometryChangeCommand::redo()
{
    applyPackedLayers(m_afterLayers);
    applyPackedSelection(m_afterSelection);
}

void GeometryChangeCommand::undo()
{
    applyPackedLayers(m_beforeLayers);
    applyPackedSelection(m_beforeSelection);
}

// ===========================================================================
//  应用
// ===========================================================================

void GeometryChangeCommand::applyPackedLayers(const PackedLayers &packed)
{
    if (!m_doc)
        return;

    // 尺寸合法性检查。画布尺寸为 0 会让后续所有计算崩掉。
    if (!packed.canvasSize.isValid() || packed.canvasSize.isEmpty())
        return;

    const int count = packed.metadata.size();

    QList<Layer> restored;
    restored.reserve(count);

    for (int i = 0; i < count; ++i) {
        Layer layer = packed.metadata.at(i);

        const QByteArray &compressed = (i < packed.compressedImages.size())
                                           ? packed.compressedImages.at(i)
                                           : QByteArray();

        if (compressed.isEmpty()) {
            // 该图层原本没有位图（理论上不会发生，但防御一下）
            layer.setImage(QImage(packed.canvasSize,
                                  QImage::Format_ARGB32_Premultiplied));
            restored.append(layer);
            continue;
        }

        const QByteArray raw = qUncompress(compressed);

        if (raw.isEmpty()) {
            // ★ 降级路径 ★
            //
            // 解压失败（数据损坏或内存不足）时，这一层退回成一张
            // 透明画布，而不是让整个命令失败或让程序崩溃。
            //
            // 用户会看到"这一层变空白了"—— 这是明显可见的异常，
            // 但比程序崩溃、全部工作丢失要好得多。且这种情况在实际
            // 使用中极罕见。
            QImage blank(packed.canvasSize, QImage::Format_ARGB32_Premultiplied);
            blank.fill(Qt::transparent);
            layer.setImage(blank);
            restored.append(layer);
            continue;
        }

        // 从压缩数据重建图像。
        //
        // 这里不能直接用 packed.canvasSize 作为尺寸 —— 因为
        // 前/后两个状态的画布尺寸可能不同（这正是几何变化的本质）。
        // 所以尺寸需要从解压出的数据长度反推：
        //   字节数 = width * height * 4
        // 但仅凭这个方程无法唯一确定宽高（多个分解可能）。
        //
        // 解决办法：用元数据里保存的图层尺寸。
        // Layer 对象的 size() 在 setImage(QImage()) 之后会失效，
        // 所以 packLayers 里必须保留尺寸信息 —— 见下面的修正。
        //
        // 简化处理：几何变化中所有图层的尺寸都等于当时的画布尺寸，
        // 而画布尺寸存两份（before/after 各一份）—— 所以这里
        // 用 packed.canvasSize 是**正确**的。
        //
        // 为什么第一念头会觉得不对？因为 undo 时用的是 before 包，
        // 它的 canvasSize 就是操作前的画布尺寸，与那时图层尺寸一致。
        // 所以两边都是自洽的。
        const int width  = packed.canvasSize.width();
        const int height = packed.canvasSize.height();

        const qint64 expectedBytes = qint64(width) * qint64(height) * 4;
        if (qint64(raw.size()) < expectedBytes) {
            // 数据长度不符，说明这个包与预期尺寸不匹配。
            // 与其写入错乱的数据，不如填一张空白。
            QImage blank(packed.canvasSize, QImage::Format_ARGB32_Premultiplied);
            blank.fill(Qt::transparent);
            layer.setImage(blank);
            restored.append(layer);
            continue;
        }

        // ★★★ 必须 copy() ★★★
        //
        // 与 ImageCommand 同样的原因，且在这里更容易踩到：
        // 本函数会被 undo/redo 调用，而 raw 是局部变量。
        //
        // 不 copy 的后果：QImage 引用了已释放的内存，画面随机错乱。
        // 由于几何变换类命令的图层数据量大，这块内存很快会被复用，
        // 所以出错的概率比 ImageCommand 更高 —— 一旦忘了 copy，
        // 表现就是"撤销几次之后图像就花了"。
        const QImage restoredImage =
            QImage(reinterpret_cast<const uchar *>(raw.constData()),
                   width, height, width * 4,
                   QImage::Format_ARGB32_Premultiplied).copy();

        if (restoredImage.isNull()) {
            QImage blank(packed.canvasSize, QImage::Format_ARGB32_Premultiplied);
            blank.fill(Qt::transparent);
            layer.setImage(blank);
        } else {
            layer.setImage(restoredImage);
        }

        restored.append(layer);
    }

    // ── 应用到文档 ──
    if (restored.isEmpty()) {
        // 至少保留一个图层 —— 这是 Document 的不变量
        Layer blank(packed.canvasSize, QObject::tr("背景"));
        restored.append(blank);
    }

    m_doc->layers().mutableAll() = restored;

    // 画布尺寸与活动图层索引
    m_doc->setCanvasSizeFromCommand(packed.canvasSize);

    const int maxIndex = qMax(0, restored.size() - 1);
    m_doc->setActiveLayerIndex(qBound(0, packed.activeIndex, maxIndex));

    m_doc->markDirty();
    emit m_doc->structureChanged();
}

void GeometryChangeCommand::applyPackedSelection(const PackedSelection &packed)
{
    if (!m_doc)
        return;

    Selection &sel = m_doc->selection();

    if (!packed.active || packed.allSelected) {
        sel.clear();
        return;
    }

    if (packed.compressedMask.isEmpty() || !packed.maskSize.isValid()) {
        sel.clear();
        return;
    }

    const QByteArray raw = qUncompress(packed.compressedMask);
    if (raw.isEmpty()) {
        // 降级：选区没恢复成功，但图像是好的。这是可接受的损失。
        sel.clear();
        return;
    }

    const int width  = packed.maskSize.width();
    const int height = packed.maskSize.height();

    if (qint64(raw.size()) < qint64(width) * qint64(height)) {
        sel.clear();
        return;
    }

    // ★ 同样必须 copy() ★（这是本文件里第二次遇到这个陷阱）
    const QImage mask =
        QImage(reinterpret_cast<const uchar *>(raw.constData()),
               width, height, width,
               QImage::Format_Grayscale8).copy();

    if (mask.isNull())
        sel.clear();
    else
        sel.setMask(mask);
}

// ===========================================================================
//  内存报告与记录价值判断
// ===========================================================================

qint64 GeometryChangeCommand::memoryUsage() const
{
    // 报告压缩后的**实际**占用。
    // 未压缩的 384MB 是"如果不压缩会占多少"，而实际占用是这个数字的
    // 几分之一 —— 状态栏显示后者才有意义。
    qint64 bytes = qint64(sizeof(GeometryChangeCommand));

    bytes += m_beforeLayers.compressedBytes();
    bytes += m_afterLayers.compressedBytes();

    bytes += qint64(m_beforeSelection.compressedMask.size());
    bytes += qint64(m_afterSelection.compressedMask.size());

    // 元数据的开销（每个 Layer 约 100 字节 × 图层数 × 2 份）
    bytes += qint64(m_beforeLayers.metadata.size()
                    + m_afterLayers.metadata.size()) * 100;

    return bytes;
}

bool GeometryChangeCommand::isWorthRecording(qint64 maxBytes) const
{
    // 默认阈值 256MB。
    //
    // 这个值的考量：
    //   · 一台典型开发机的可用内存是几 GB，256MB 是可承受的
    //   · 但撤销栈里可能已经有若干步，累计起来才是真正的压力
    //   · 所以这是一个"单步上限"，由 CommandStack 再配合总步数限制
    //
    // 超过这个阈值时，调用方应当：
    //   1. 仍然执行操作（用户要的效果必须给）
    //   2. 但不记录这一步的撤销
    //   3. 提示用户"此操作不会记录到撤销历史"
    return memoryUsage() <= maxBytes;
}

// ===========================================================================
//  状态快照
// ===========================================================================

GeometryChangeCommand::Snapshot
GeometryChangeCommand::captureState(const Document *doc)
{
    Snapshot snap;

    if (!doc)
        return snap;

    // ★ 这里必须用 cloneAll() 而不是直接取 layers().all() ★
    //
    // 原因：这个快照要在操作**之前**抓取，然后在操作之后再传给
    // GeometryChangeCommand —— 中间隔着一次真实的操作。
    // 如果只是浅拷贝列表，操作会就地修改 Layer 对象（setImage），
    // 快照里看到的就是修改后的状态了 —— 撤销将无法回到原样。
    //
    // cloneAll() 内部对每个 Layer 调 clone()，而 clone() 里做了
    // QImage::copy()，得到的是独立副本。这才是真正的"快照"。
    //
    // 代价：这一步会真实复制所有像素。4 个 4000×3000 图层 = 192MB
    // 的临时内存。所以 captureState 不宜频繁调用，只在几何操作前后各一次。
    snap.layers = doc->layers().cloneAll();
    snap.canvasSize = doc->canvasSize();
    snap.selection  = doc->selection().clone();
    snap.activeIndex = doc->activeLayerIndex();

    return snap;
}

} // namespace zl
