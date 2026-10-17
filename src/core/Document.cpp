#include "core/Document.h"
#include "compat/QtCompat.h"

#include "engine/Blend.h"
#include "engine/Resample.h"
#include "engine/Transform.h"
#include "engine/Adjust.h"
#include "engine/Filters.h"

#include <QFileInfo>
#include <QPainter>
#include <QtMath>

namespace zl {

// ===========================================================================
//  构造 / 析构
// ===========================================================================

Document::Document(const QSize &size, const QColor &fillColor, QObject *parent)
    : QObject(parent)
{
    // 画布尺寸必须有下限。0 或负数尺寸会让后续所有计算失效
    // （除零、负的 scanLine 索引等），这里统一兜底到 1×1。
    m_canvasSize = (size.isValid() && !size.isEmpty()) ? size : QSize(1, 1);

    createInitialLayer(fillColor);

    // 选区蒙版必须与画布尺寸一致，否则所有选区运算都会因为尺寸不匹配
    // 而静默失败（Selection 里做了尺寸检查，不匹配就直接 return）。
    m_selection.clear();
}

Document::~Document() = default;

void Document::createInitialLayer(const QColor &fillColor)
{
    Layer layer(m_canvasSize, tr("Background"));

    // fillColor 为透明时不需要额外操作（Layer 构造已填透明）；
    // 其他颜色需要显式填充。
    if (fillColor.alpha() != 0) {
        QImage &img = layer.mutableImage();
        if (!img.isNull()) {
            // 注意用 CompositionMode_Source 而不是默认的 SourceOver。
            // SourceOver 会把新颜色"叠"在透明背景上，虽然结果看起来相同，
            // 但半透明填充色时会有差异（Source 是直接替换）。
            QPainter painter(&img);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(img.rect(), fillColor);
            painter.end();
        }
    }

    m_layers.append(layer);
    m_activeLayerIndex = 0;
}

// ===========================================================================
//  图层
// ===========================================================================

void Document::setActiveLayerIndex(int index)
{
    if (!m_layers.isValidIndex(index))
        return;
    if (m_activeLayerIndex == index)
        return;

    m_activeLayerIndex = index;
    emit activeLayerChanged(index);
}

Layer *Document::activeLayer()
{
    return m_layers.layer(m_activeLayerIndex);
}

const Layer *Document::activeLayer() const
{
    return m_layers.layer(m_activeLayerIndex);
}

int Document::addLayer(const QString &name)
{
    // 新图层插入在活动图层**上方**（索引 + 1），这是 Photoshop 的行为。
    // 用户更习惯"在当前图层上面新建"。
    const int insertAt = m_activeLayerIndex + 1;

    // 自动命名：若调用方没给名字，用"图层 N"。
    // N 取当前图层数+1，不追求绝对唯一（用户重命名后可能重复，
    // 那是用户的自由，不必强制）。
    const QString layerName = name.isEmpty()
                                  ? tr("Layer %1").arg(m_layers.count() + 1)
                                  : name;

    Layer layer(m_canvasSize, layerName);
    const int index = m_layers.insert(qBound(0, insertAt, m_layers.count()), layer);

    m_activeLayerIndex = index;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return index;
}

int Document::addLayer(const Layer &layer, int index)
{
    // 尺寸对齐：粘贴进来的图层可能尺寸与画布不同。
    // 这里统一到画布尺寸 —— 保证"所有图层尺寸一致"这个不变量，
    // 后续合成逻辑才能简化。若内容更大，则扩大画布（而不是裁掉内容）。
    Layer adapted = layer;

    if (adapted.size() != m_canvasSize) {
        // 若内容超出画布，先扩大画布，避免粘贴时内容被裁
        if (adapted.width() > m_canvasSize.width()
            || adapted.height() > m_canvasSize.height()) {
            const QSize newSize(qMax(m_canvasSize.width(),  adapted.width()),
                                qMax(m_canvasSize.height(), adapted.height()));
            setCanvasSizeInternal(newSize);
        }

        // 把内容放到画布左上角，不足部分透明
        QImage placed(m_canvasSize, QImage::Format_ARGB32_Premultiplied);
        placed.fill(Qt::transparent);
        {
            QPainter painter(&placed);
            painter.drawImage(0, 0, adapted.image());
            painter.end();
        }
        adapted.setImage(placed);
    }

    const int actualIndex = (index < 0)
                                ? m_layers.count()
                                : qBound(0, index, m_layers.count());

    const int result = m_layers.insert(actualIndex, adapted);
    m_activeLayerIndex = result;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return result;
}

bool Document::removeLayer(int index)
{
    if (!m_layers.isValidIndex(index))
        return false;

    // 最后一个图层不能删 —— Document 必须始终至少有一个图层。
    // 这条约束看起来可以放宽（删完了自动新建一个空的），但那样会让
    // "删除最后一个图层"这个动作产生一个意外的撤销步骤，用户体验更差。
    // 直接从菜单层面禁用该项即可。
    if (m_layers.count() <= 1)
        return false;

    if (!m_layers.remove(index))
        return false;

    // 修正活动图层索引
    if (m_activeLayerIndex >= m_layers.count())
        m_activeLayerIndex = m_layers.count() - 1;
    else if (m_activeLayerIndex > index)
        --m_activeLayerIndex;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return true;
}

bool Document::duplicateLayer(int index)
{
    const Layer *src = m_layers.layer(index);
    if (!src)
        return false;

    // clone() 会做 QImage::copy()，是真正的深拷贝
    Layer copy = src->clone();
    copy.setName(tr("%1 Copy").arg(src->name()));

    // 副本放在原图层上方
    const int newIndex = m_layers.insert(index + 1, copy);
    m_activeLayerIndex = newIndex;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return true;
}

bool Document::moveLayer(int from, int to)
{
    if (!m_layers.move(from, to))
        return false;

    // 活动图层跟随移动
    if (m_activeLayerIndex == from)
        m_activeLayerIndex = to;
    else if (from < m_activeLayerIndex && to >= m_activeLayerIndex)
        --m_activeLayerIndex;
    else if (from > m_activeLayerIndex && to <= m_activeLayerIndex)
        ++m_activeLayerIndex;

    m_activeLayerIndex = qBound(0, m_activeLayerIndex, m_layers.count() - 1);

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return true;
}

bool Document::mergeLayerDown(int index)
{
    // ========================================================================
    //  ★ 方向：索引 0 是最底部，索引越大越靠上 ★
    // ------------------------------------------------------------------------
    //  "向下合并" = 把 index 这一层合并进它**下面**的那层（index - 1），
    //  结果留在 index - 1 的位置，并且承接下层图层的名字与属性。
    //  这既是 Photoshop 的 Ctrl+E 语义，也是用户对"向下"两个字的直觉。
    //
    //  ⚠ 实测踩过（阶段五）——原始实现把方向写反了：
    //      `upper = layer(index + 1); lower = layer(index);`
    //    也就是"把上面那层合成到当前层上"，结果落在 index。
    //    症状有两层，而且互相掩护，所以一直没被发现：
    //      · 最**上面**的图层反而不让合并（index+1 越界）——
    //        而菜单那句守卫恰好也是"最底部的图层无法向下合并"，
    //        守卫与实现"看起来都对"，其实一个管着另一头；
    //      · 合并后 MainWindow 传下来的新活动索引是 index - 1，
    //        但结果其实在 index —— 于是"合并完选中了旁边那个图层"。
    //    两处错误各自都"像是合理的"，谁也没报错、谁也没崩溃。
    if (!m_layers.isValidIndex(index) || !m_layers.isValidIndex(index - 1))
        return false;

    Layer *upper = m_layers.layer(index);       // 被吃掉的那层（在上面）
    Layer *lower = m_layers.layer(index - 1);   // 承接结果的那层（在下面）
    if (!upper || !lower)
        return false;

    // ── 用合成函数合并两层 ──
    //
    // 不能简单地"画上去"，因为上面的图层可能有混合模式与不透明度。
    // 复用 compositeLayer 能保证合并结果与用户看到的画面一致。
    //
    // ★ 下层自己的**不透明度**必须先烘进底图 ★
    //   合成栈的规则是 `canvas = compositeLayer(canvas, img_i, mode_i, op_i)`，
    //   所以"下层"在整条栈里的贡献并不是它裸的位图，而是**乘过 op 的位图**。
    //   如果直接把裸位图当 base，再给结果图层挂上下层的 op，就会双重衰减：
    //   用户把 50% 的上层与 50% 的下层合并，画面会突然透出更下面的东西。
    //   正确做法是把 op 烘进 base，结果图层则回到 100% 不透明 ——
    //   因为"下层的不透明度"已经变成像素里的一部分了。
    //
    //   顺带一个便利：`compositeLayer(透明底, L, Normal, op)` 本身就是
    //   "把 L 的 alpha 乘上 op"，不需要另写一个缩放函数。
    QImage scaledBase(m_canvasSize, QImage::Format_ARGB32_Premultiplied);
    scaledBase.fill(Qt::transparent);
    const QImage base = blend::compositeLayer(scaledBase,
                                              layerPixelsForComposite(*lower),
                                              BlendMode::Normal,
                                              lower->opacityF());

    const QImage merged = blend::compositeLayer(base,
                                                layerPixelsForComposite(*upper),
                                                upper->blendMode(),
                                                upper->opacityF());

    if (merged.isNull())
        return false;

    // 结果图层继承下层的位置、名字与可见性。
    //
    // ★ 混合模式重置为"正常"、不透明度重置为 100% ★
    //   上层的模式/不透明度在上一句里已经烘进像素了；
    //   下层的不透明度烘进了 base。所以这里必须清成中性值，
    //   否则就成了"烘一次、再乘一次"。
    //
    //   ⚠ 已知限制（明确写下来，免得将来被当成新 bug 查）★
    //     如果**下层**自己用的不是"正常"混合模式，它的模式没法烘进像素
    //     （模式要拿背景色参与运算，而背景在渲染时才知道），
    //     这一种组合下合并前后的画面会有细微差别。
    //     Photoshop 对合并也留有同类限制。其余组合都是逐像素精确的。
    Layer newLayer = lower->clone();
    newLayer.setImage(merged);
    newLayer.setBlendMode(BlendMode::Normal);
    newLayer.setOpacity(255);

    // 用"替换两个图层为一个"的方式，保证撤销时能完整恢复
    m_layers.mutableAll()[index - 1] = newLayer;
    m_layers.mutableAll().removeAt(index);

    // ★ 活动图层要显式跟着走，不能只做越界钳制 ★
    //   活动图层原本就在 index（被吃掉的那层）时，删掉 index 之后
    //   索引原地不动 —— 而原地指向的已经是"原来上面那层"了，
    //   用户看到的现象是"合并完选中了不相干的一个图层"。
    if (m_activeLayerIndex >= index)
        --m_activeLayerIndex;
    if (m_activeLayerIndex >= m_layers.count())
        m_activeLayerIndex = m_layers.count() - 1;
    if (m_activeLayerIndex < 0)
        m_activeLayerIndex = 0;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return true;
}

bool Document::mergeVisibleLayers()
{
    // 统计可见图层数量。只有一个可见图层时没什么可合并的。
    int visibleCount = 0;
    for (int i = 0; i < m_layers.count(); ++i) {
        if (m_layers.layer(i) && m_layers.layer(i)->isVisible())
            ++visibleCount;
    }

    if (visibleCount < 2)
        return false;

    // ── 把可见图层合成到一张图上 ──
    QList<QImage>    images;
    QList<BlendMode> modes;
    QList<qreal>     opacities;
    QList<bool>      visible;

    for (int i = 0; i < m_layers.count(); ++i) {
        const Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;
        images.append(layerPixelsForComposite(*layer));
        modes.append(layer->blendMode());
        opacities.append(layer->opacityF());
        visible.append(layer->isVisible());
    }

    const QImage merged = blend::compositeLayers(images, modes, opacities,
                                                 visible, m_canvasSize,
                                                 Qt::transparent);
    if (merged.isNull())
        return false;

    // ── 结果放在最上面那个可见图层的位置 ──
    // 不可见的图层保持原样（这也是 Photoshop 的行为：合并可见图层
    // 不会删除隐藏图层）。
    int insertAt = -1;
    for (int i = m_layers.count() - 1; i >= 0; --i) {
        if (m_layers.layer(i) && m_layers.layer(i)->isVisible()) {
            insertAt = i;
            break;
        }
    }

    if (insertAt < 0)
        return false;

    Layer mergedLayer(m_canvasSize, tr("Merged Layer"));
    mergedLayer.setImage(merged);

    // ★ 插入位置：留在原"最上面那个可见图层"的槽位上 ★
    //
    //  删掉可见图层之后，剩下的只有隐藏图层，且保持原来的相对顺序。
    //  新图层该插在"原来 insertAt 位置之前的那批隐藏图层"之后 ——
    //  也就是数一遍 **原索引** < insertAt 的隐藏图层有几个。
    //
    //  ⚠ 实测踩过两次，两次都是**索引算在了错误的时刻** ★
    //
    //    第一次（原始实现）：直接 `append()` 到栈顶，注释还写着"实际效果一致"。
    //      当时看确实一致 —— 隐藏图层不参与绘制，合并在最上面还是原位，
    //      画面完全一样。但图层顺序已经错了：栈顶如果有隐藏图层，
    //      合并结果就骑到了它头上。用户随后把那个隐藏图层打开，
    //      叠加次序就和他记忆里的不一样了。
    //
    //    第二次（本次修这个 bug 时的第一版修复）：把"数隐藏层"那段写在了
    //      **删除之后**，却仍然拿 insertAt 当上界 —— 而 insertAt 是**删除前**
    //      的索引。删除会让后面的索引整体前移，于是数出来的东西完全不对。
    //      这次是 tests/mainwin_test.cpp [9] 组当场抓住的
    //      （报"期望 合并图层，实际 隐藏顶"）。
    //
    //  结论：只要一段代码里同时有"按原索引读"和"改列表长度"，
    //        就必须先把所有按原索引的读数取完，再动列表。
    int hiddenBelow = 0;
    for (int i = 0; i < insertAt && i < m_layers.count(); ++i) {
        if (m_layers.layer(i) && !m_layers.layer(i)->isVisible())
            ++hiddenBelow;
    }

    // 删除所有可见图层（从后往前删，避免索引偏移）
    for (int i = m_layers.count() - 1; i >= 0; --i) {
        if (m_layers.layer(i) && m_layers.layer(i)->isVisible())
            m_layers.mutableAll().removeAt(i);
    }

    // hiddenBelow 一定落在 [0, 剩余数量] 内（它数的是"存在且隐藏"的数量）
    m_layers.mutableAll().insert(qBound(0, hiddenBelow, m_layers.count()),
                                 mergedLayer);
    m_activeLayerIndex = qBound(0, hiddenBelow, m_layers.count() - 1);

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(m_activeLayerIndex);
    return true;
}

bool Document::flattenImage()
{
    if (m_layers.count() <= 1)
        return false;

    // 拼合图像 = 合并所有可见图层 + 丢弃不可见图层
    QList<QImage>    images;
    QList<BlendMode> modes;
    QList<qreal>     opacities;
    QList<bool>      visible;

    for (int i = 0; i < m_layers.count(); ++i) {
        const Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;
        images.append(layerPixelsForComposite(*layer));
        modes.append(layer->blendMode());
        opacities.append(layer->opacityF());
        // 关键区别：拼合图像会**丢弃**不可见图层的内容，
        // 所以这里把可见性全部视为 true —— 被隐藏的图层不参与最终结果。
        visible.append(layer->isVisible());
    }

    const QImage merged = blend::compositeLayers(images, modes, opacities,
                                                 visible, m_canvasSize,
                                                 Qt::transparent);
    if (merged.isNull())
        return false;

    // 拼合后只剩一个图层，选区也失去意义（它的蒙版尺寸还匹配，
    // 但语义上不再对应任何东西），这里保守地保留选区 ——
    // 用户可能还想用它做别的操作。
    m_layers.clear();

    Layer flat(m_canvasSize, tr("Background"));
    flat.setImage(merged);
    m_layers.append(flat);

    m_activeLayerIndex = 0;

    markDirty();
    emit structureChanged();
    emit activeLayerChanged(0);
    return true;
}

// ===========================================================================
//  合成
// ===========================================================================

void Document::markDirty()
{
    // ★ 全量失效：语义 = "整画布都脏了" ★
    //   这是正确性的最终保证，**永久保留**。任何"不确定这次改了什么"
    //   的调用点都用它，行为与引入脏矩形之前完全一致。
    m_compositeValid = false;
    m_contentBoundsValid = false;

    // ★ 必须把全量标志也立起来 ★
    //   它会让上一次累积的局部脏矩形失效。否则 composite() 可能
    //   拿着一个"小块脏矩形"走快路径，而实际整张图都变了 ——
    //   症状是画面大部分区域停在旧内容上。
    //   这一条正是"markDirty() 后紧跟 markDirtyRegion(小块)"必须
    //   仍然整图重算的原因，perf 自检里有专门的兜底断言。
    m_fullInvalidate = true;
    m_dirtyRegion = QRect();

    ++m_revision;
    emit contentChanged();
}

void Document::markDirtyRegion(const QRect &region)
{
    // 局部失效：只有这一块像素变了（图像坐标）。
    //
    // ★ region 为空怎么办？按全量失效处理，不赌"也许真的什么都没变" ★
    //   调用方传空矩形，要么是"什么都没变"（那就不该调这个函数），
    //   要么是算错了（例如忘了与画布求交、或者脏矩形在画布之外）。
    //   两种情况下按全量失效都是安全的，最多白算一次。
    //   这条"算不出来就保守"的规矩，是脏矩形机制唯一的安全阀。
    if (region.isEmpty()) {
        markDirty();
        return;
    }

    m_compositeValid = false;
    m_contentBoundsValid = false;   // 在空白处画一笔会把内容包围盒撑大

    // ★ 已经是全量失效时保持全量，不要被一个局部矩形"降级" ★
    //   （标志位本身不用动，这里只是把"为什么"写清楚）
    if (!m_fullInvalidate)
        m_dirtyRegion = m_dirtyRegion.united(region);

    ++m_revision;
    emit contentChanged();
}

QRect Document::pendingDirtyRegion() const
{
    // 空 QRect = 整画布都脏（约定见头文件 —— 调用方必须按全图处理）
    if (m_fullInvalidate)
        return QRect();

    return m_dirtyRegion;
}

QImage Document::layerPixelsForComposite(const Layer &layer) const
{
    if (!layer.hasMask())
        return layer.image();

    // DestinationIn：结果 = 目标 × 源 alpha，正好是"蒙版按比例挡住图层"。
    // 逐像素手算也能做，但边缘会多一次除以 255 的舍入，而且慢一个量级。
    QImage masked = layer.image().copy();
    QPainter painter(&masked);
    if (!painter.isActive())
        return layer.image();
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, layer.mask());
    painter.end();
    return masked;
}

QImage Document::composite()
{
    // 缓存命中：直接返回。
    // 这里的缓存是"整画布缓存"，paintEvent 可能每秒被调用几十次
    // （滚动、鼠标移动触发重绘），没有缓存的话每次都要重新合成 4 个
    // 4000×3000 的图层，完全无法交互。
    if (m_compositeValid && !m_compositeCache.isNull())
        return m_compositeCache;

    QList<QImage>    images;
    QList<BlendMode> modes;
    QList<qreal>     opacities;
    QList<bool>      visible;

    images.reserve(m_layers.count());
    for (int i = 0; i < m_layers.count(); ++i) {
        const Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;
        // 有蒙版的图层要取"蒙版已乘进 alpha"的那一份。
        images.append(layerPixelsForComposite(*layer));
        modes.append(layer->blendMode());
        opacities.append(layer->opacityF());
        visible.append(layer->isVisible());
    }

    // ── 快路径：只重算脏矩形，再把补丁贴回缓存 ──
    //
    // 三个条件必须同时满足：
    //   · 不是全量失效
    //   · 缓存已存在，且尺寸仍与画布一致 —— 否则"贴回"会贴到错误的坐标系上
    //     （画布尺寸变化一定会调 markDirty()，所以这只是双保险）
    //   · 脏矩形与画布求交后非空
    if (!m_fullInvalidate && !m_compositeCache.isNull()
        && m_compositeCache.size() == m_canvasSize) {

        const QRect region = m_dirtyRegion.intersected(canvasRect());

        if (!region.isEmpty()) {
            const QImage patch = blend::compositeLayersRegion(
                images, modes, opacities, visible, m_canvasSize,
                region, Qt::transparent);

            if (!patch.isNull()) {
                // ★ "贴回"这一步有副作用，所以只能发生在 core 层 ★
                //   engine 层是"无副作用纯函数"，它只负责算出补丁；
                //   把它写进缓存是 Document 的职责。这条边界不能破 ——
                //   否则 engine 就不再能被离线自检直接调用了。
                QPainter patcher(&m_compositeCache);
                if (patcher.isActive()) {
                    // Source 而不是 SourceOver：我们要的是"把这块**替换**成
                    // 补丁"，包括补丁里变透明的像素。SourceOver 会把透明
                    // 当作"不覆盖"，于是擦掉的内容擦不掉（同一个坑在
                    // BrushTool 写回处也踩过）。
                    patcher.setCompositionMode(QPainter::CompositionMode_Source);
                    patcher.drawImage(region.topLeft(), patch);
                    patcher.end();

                    m_compositeValid = true;
                    m_fullInvalidate = false;
                    m_dirtyRegion = QRect();
                    ++m_regionCompositeCount;
                    return m_compositeCache;
                }
            }
            // 补丁算不出来（内存不足等）→ 掉到下面的全量路径，
            // 而不是返回一个"贴了一半"的缓存
        }
    }

    // ── 兜底路径：整图重算 ──
    m_compositeCache = blend::compositeLayers(images, modes, opacities,
                                              visible, m_canvasSize,
                                              Qt::transparent);
    m_compositeValid = true;
    m_fullInvalidate = false;
    m_dirtyRegion = QRect();
    ++m_fullCompositeCount;

    return m_compositeCache;
}

// ===========================================================================
//  内容包围盒
// ===========================================================================

QRect Document::contentBounds() const
{
    if (m_contentBoundsValid)
        return m_contentBoundsCache;

    // 求所有图层包围盒的并集。
    // 注意这里用 transform::contentBounds 逐图层算，而不是先把所有图层
    // 合成再算一次 —— 后者的成本是"合成 + 遍历"，前者是"遍历各层"，
    // 在图层少但画布大时前者更快，而且不需要合成缓存失效。
    QRect result;

    for (int i = 0; i < m_layers.count(); ++i) {
        const Layer *layer = m_layers.layer(i);
        if (!layer || !layer->isVisible())
            continue;

        const QRect layerBounds = transform::contentBounds(layer->image());
        if (layerBounds.isEmpty())
            continue;

        result = result.isNull() ? layerBounds : result.united(layerBounds);
    }

    m_contentBoundsCache = result;
    m_contentBoundsValid = true;
    return result;
}

// ===========================================================================
//  几何操作
// ===========================================================================

void Document::setCanvasSizeInternal(const QSize &size)
{
    if (size == m_canvasSize)
        return;

    m_canvasSize = size;
    emit canvasSizeChanged(size);
}

void Document::setCanvasSizeFromCommand(const QSize &size)
{
    // 只改尺寸字段，不碰图层位图。
    // 详见头文件里的说明 —— 这是 GeometryChangeCommand 的专用入口。
    setCanvasSizeInternal(size);

    // 选区蒙版必须与画布尺寸一致，否则选区运算会静默失效。
    // 这里不传 offset —— offset 的处理已由命令快照本身完成
    // （快照里存的蒙版就是目标尺寸的）。
    if (!m_selection.isActive()) {
        // 无选区状态下，只需保证蒙版被清空（尺寸一致性自动满足）
        m_selection.clear();
    }
}

void Document::syncSelectionToCanvas(const QPoint &offset)
{
    // 选区蒙版的原点始终与画布原点对齐，尺寸必须一致。
    // 画布变化后必须调用这个函数，否则选区运算会因为尺寸不匹配而静默失效。
    m_selection.resizeCanvas(m_canvasSize, offset);
}

bool Document::scaleImage(const QSize &newSize, ResampleMethod method)
{
    if (!newSize.isValid() || newSize.isEmpty())
        return false;
    if (newSize == m_canvasSize)
        return false;

    // ── 逐图层重采样 ──
    //
    // 这里对每个图层独立重采样，而不是"先合成、缩放、再还原到图层"。
    // 前者是正确的：每个图层的内容质量都得到保留（例如文字图层的
    // 锐利边缘在缩放后仍然锐利），而后者会把所有图层烘焙成一张位图。
    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        const QImage scaled = resample::scale(layer->image(), newSize, method);
        if (scaled.isNull())
            return false;   // 内存不足等情况下中止

        layer->setImage(scaled);
    }

    setCanvasSizeInternal(newSize);

    // 选区也要缩放。这里用"位移为零"的方式调用 ——
    // 严格来说选区蒙版也应该按比例重采样，但那会让羽化边缘变形。
    // 标准版的做法是：缩放后清空选区，让用户重新选。
    // 这样更符合"图像尺寸变了，原来的选区没有意义"的直觉。
    m_selection.clear();

    markDirty();
    return true;
}

bool Document::resizeCanvas(const QSize &newSize, Anchor anchor,
                            const QColor &fillColor, bool smart)
{
    if (!newSize.isValid() || newSize.isEmpty())
        return false;
    if (newSize == m_canvasSize)
        return false;

    // ── 计算偏移 ──
    //
    // 偏移的语义：新坐标 = 旧坐标 + offset。
    //   offset > 0 表示内容往右下移动（画布向左上扩大）
    //
    // ★ 走 engine 的共用函数，不在这里重写策略 ★
    //   画布大小对话框的预览示意图必须画出"将要发生的偏移"，如果那边
    //   自己复制一份策略，两边迟早分叉 —— 表现为"预览说不会裁切，
    //   确定后却发现被裁掉了"。宁可多一个函数，不要两份策略。
    //
    //   内容包围盒而非整个旧画布：用户关心的是"我画的东西别丢"，
    //   透明边缘丢了无所谓。内容为空（全透明）时的兜底也在那个函数里。
    const QRect content = contentBounds();
    const QPoint offset = canvasResizeOffset(content, m_canvasSize, newSize,
                                             anchor, smart);

    // ── 逐图层重排 ──
    //
    // 这个操作**不做重采样**，所以是无损的。
    // 实现上用 QPainter 绘制到新尺寸的透明画布上 ——
    // 因为我们预先算好了 offset，所以不需要插值，Qt 会做整数像素搬运。
    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        QImage result(newSize, QImage::Format_ARGB32_Premultiplied);

        // 填充底色：画布扩大时新增区域应该是什么颜色。
        // 注意这里用 Source 语义的 fill，所以会覆盖整个画布，
        // 而不是只在透明处填充。
        result.fill(fillColor);

        {
            QPainter painter(&result);
            // SourceOver：把原内容叠到底色上。
            // 若用 Source，原内容的透明区域会擦掉底色。
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            // 明确关闭平滑 —— 这是纯位移，任何插值都会让图像变糊。
            // 这是一个容易被忽略但很致命的点：QPainter 默认在缩放时才平滑，
            // 但某些平台/组合下整数平移也可能走插值路径。
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(offset.x(), offset.y(), layer->image());
            painter.end();
        }

        layer->setImage(result);
    }

    setCanvasSizeInternal(newSize);

    // 选区跟随画布变换
    syncSelectionToCanvas(offset);

    markDirty();
    return true;
}

bool Document::rotateImage90(bool clockwise)
{
    // 90° 旋转是无损的 —— 这里不应该出现任何插值。
    // 验证方法：连续旋转 4 次应得到与原图逐像素相同的结果。
    const int quarterTurns = clockwise ? 1 : 3;   // 3 = 逆时针 90

    // 新画布尺寸：宽高互换
    const QSize newSize(m_canvasSize.height(), m_canvasSize.width());

    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        const QImage rotated = rotateQuarterTurns(layer->image(), quarterTurns);
        if (rotated.isNull())
            return false;

        layer->setImage(rotated);
    }

    setCanvasSizeInternal(newSize);

    // 选区也同步旋转。这里简化为清空 —— 让用户重新选，
    // 而不是实现一套蒙版旋转（用户很难判断"选区跟着转了"是不是预期行为）。
    m_selection.clear();

    markDirty();
    return true;
}

bool Document::rotateImageArbitrary(qreal angleDeg, ResampleMethod method,
                                    const QColor &fillColor)
{
    // 任意角度旋转会改变画布尺寸（扩大到容纳旋转后的内容）。
    //
    // ★ 为什么必须先判断 90° 倍数 ★
    //   如果用户输入 90，浮点运算后可能得到 89.9999999。
    //   不判断的话就会走插值路径，把一张本该无损旋转的图模糊一次。
    //   isRightAngle() 带容差，能正确识别这种情况。
    int quarterTurns = 0;
    if (isRightAngle(angleDeg, &quarterTurns)) {
        if (quarterTurns == 0)
            return false;   // 0 度，什么都不用做
        if (quarterTurns == 1)  return rotateImage90(true);
        if (quarterTurns == 2) {
            // 180°：宽高不变
            for (int i = 0; i < m_layers.count(); ++i) {
                Layer *layer = m_layers.layer(i);
                if (!layer)
                    continue;
                const QImage rotated = rotateQuarterTurns(layer->image(), 2);
                if (rotated.isNull())
                    return false;
                layer->setImage(rotated);
            }
            m_selection.clear();
            markDirty();
            return true;
        }
        return rotateImage90(false);   // 270°
    }

    // ── 任意角度 ──
    const QSize newSize = rotatedBoundsSize(m_canvasSize, angleDeg);

    // ★ 旋转围绕原点，内容中心也会跟着转走 ★
    //   所以偏移要用"旋转之后的内容中心"来算，否则内容会偏出画布被裁掉
    //   一角，而四角的填充区大小不均。（详细说明见 Geometry.cpp 的
    //   rotateImage()：那是一次真实的踩坑记录。）
    QTransform rotation;
    rotation.rotate(angleDeg);

    const QPointF oldCenter(m_canvasSize.width() / 2.0, m_canvasSize.height() / 2.0);
    const QPointF newCenter(newSize.width() / 2.0, newSize.height() / 2.0);
    const QPointF delta = newCenter - rotation.map(oldCenter);

    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        GeometryParams params;
        params.transform = rotation;
        params.newCanvasSize = newSize;
        params.interpolation = method;
        params.fillColor = fillColor;
        params.contentOffset = QPoint(int(qRound(delta.x())), int(qRound(delta.y())));

        const QImage rotated = applyGeometry(layer->image(), params);
        if (rotated.isNull())
            return false;

        layer->setImage(rotated);
    }

    setCanvasSizeInternal(newSize);
    m_selection.clear();
    markDirty();
    return true;
}

bool Document::flipImage(FlipDirection dir)
{
    // 图像级翻转：所有图层与选区一起翻转，画布尺寸不变。
    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        const QImage flipped = transform::flip(layer->image(), dir);
        if (flipped.isNull())
            return false;

        layer->setImage(flipped);
    }

    // 选区翻转：对蒙版做同样的镜像。
    // 这一步不能省 —— 否则用户翻转图像后，选区的形状还是原来的，
    // 会看到"选区框和内容对不上"的诡异现象。
    if (m_selection.isActive()) {
        QImage &mask = m_selection.mutableMask();
        if (!mask.isNull()) {
            const bool horizontal = (dir == FlipDirection::Horizontal);
            const bool vertical   = (dir == FlipDirection::Vertical);
            // ★ 用兼容层的 mirrorImage，而不是直接调 QImage::mirrored ★
            //   Qt6 把 mirrored() 改名成了 flipped()，且一次只能翻一个方向。
            //   兼容层把两个版本的行为统一成同一个签名，
            //   将来升级 Qt6 时只改 QtCompat.h 一处。
            mask = zl::mirrorImage(mask, horizontal, vertical);
            m_selection.markModified();
        }
    }

    markDirty();
    return true;
}

bool Document::crop(const QRect &rect)
{
    // 裁剪就是"把画布缩小到指定矩形，并把内容平移过来"。
    // 实现上复用 resizeCanvas 的路径，只是 offset 由裁剪矩形决定。
    //
    // 这里不用 resizeCanvas() 是因为它的锚点语义无法表达"裁到任意位置"——
    // 锚点只能按九宫格摆，而裁剪矩形可以是任意位置。
    const QRect validRect = rect.intersected(canvasRect());
    if (validRect.isEmpty())
        return false;
    if (validRect == canvasRect())
        return false;   // 全画布裁剪，等于没裁

    const QSize newSize = validRect.size();
    const QPoint offset(-validRect.x(), -validRect.y());

    for (int i = 0; i < m_layers.count(); ++i) {
        Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;

        QImage result(newSize, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        {
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(offset.x(), offset.y(), layer->image());
            painter.end();
        }
        layer->setImage(result);
    }

    setCanvasSizeInternal(newSize);
    syncSelectionToCanvas(offset);

    markDirty();
    return true;
}

// ===========================================================================
//  文件状态
// ===========================================================================

void Document::setFilePath(const QString &path)
{
    m_filePath = path;
}

void Document::setModified(bool modified)
{
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged(modified);
}

QString Document::displayName() const
{
    // 有文件路径时用文件名，否则用"未命名"。
    // 前面加 * 表示有未保存修改 —— 这是编辑器的通行做法，
    // 用户一眼就能看出"我改过但没保存"。
    QString base;

    if (m_filePath.isEmpty()) {
        // ★ 没有文件路径时优先用"提示名"（屏幕截图这类无磁盘来源的页）★
        //   都没有才落到"未命名"。两种情况都仍然只是**显示**，
        //   保存路径的判断只认 m_filePath。
        base = m_titleHint.isEmpty() ? tr("Untitled") : m_titleHint;
    } else {
        base = QFileInfo(m_filePath).fileName();
        if (base.isEmpty())
            base = tr("Untitled");
    }

    return m_modified ? QStringLiteral("*") + base : base;
}

qint64 Document::memoryUsage() const
{
    // 每个像素 4 字节（ARGB32_Premultiplied）
    qint64 total = 0;

    for (int i = 0; i < m_layers.count(); ++i) {
        const Layer *layer = m_layers.layer(i);
        if (!layer)
            continue;
        total += qint64(layer->size().width()) * qint64(layer->size().height()) * 4;
    }

    // 选区蒙版每个像素 1 字节
    if (m_selection.isActive() && !m_selection.mask().isNull()) {
        total += qint64(m_selection.mask().width())
               * qint64(m_selection.mask().height());
    }

    // 合成缓存（若有效）也要算上
    if (m_compositeValid && !m_compositeCache.isNull()) {
        total += qint64(m_compositeCache.width())
               * qint64(m_compositeCache.height()) * 4;
    }

    return total;
}

void Document::setDpi(qreal x, qreal y)
{
    // DPI 的合理范围：1 ~ 10000。超出的数值基本可以确定是坏数据，
    // 直接钳制而不是拒绝，因为用户可能只是手滑多打了一位。
    m_dpiX = qBound(1.0, x, 10000.0);
    m_dpiY = qBound(1.0, y, 10000.0);
}

} // namespace zl
