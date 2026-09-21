#include "core/LayerList.h"

#include <QSize>

namespace zl {

QSize LayerList::bounds() const
{
    // 画布尺寸取所有图层尺寸的并集。
    // 正常情况下所有图层尺寸都与画布一致（图像大小/画布大小操作会同步所有
    // 图层），但粘贴超大内容、或导入尺寸不符的图片时会出现不一致，
    // 这里取并集可以避免内容被无声裁掉。
    QSize result;
    for (const Layer &layer : m_layers) {
        const QSize s = layer.size();
        result.setWidth (qMax(result.width(),  s.width()));
        result.setHeight(qMax(result.height(), s.height()));
    }
    return result;
}

Layer *LayerList::layer(int index)
{
    return isValidIndex(index) ? &m_layers[index] : nullptr;
}

const Layer *LayerList::layer(int index) const
{
    return isValidIndex(index) ? &m_layers[index] : nullptr;
}

Layer *LayerList::topLayer()
{
    return m_layers.isEmpty() ? nullptr : &m_layers.last();
}

const Layer *LayerList::topLayer() const
{
    return m_layers.isEmpty() ? nullptr : &m_layers.last();
}

int LayerList::insert(int index, const Layer &layer)
{
    // 越界一律视为"追加到顶部"，而不是报错。
    // 调用方（尤其是从撤销栈恢复时）传 -1 表示"追加"会方便很多。
    if (index < 0 || index > m_layers.size())
        index = m_layers.size();

    m_layers.insert(index, layer);
    return index;
}

int LayerList::insertBelow(int index, const Layer &layer)
{
    // 在"指定图层下方"插入，也就是索引不变的位置插入新图层。
    // 若 index 无效则退化为追加到顶部。
    if (!isValidIndex(index))
        return append(layer);

    m_layers.insert(index, layer);
    return index;
}

bool LayerList::remove(int index)
{
    if (!isValidIndex(index))
        return false;

    // 最后一个图层不允许删除 —— Document 必须始终至少有一个图层，
    // 否则合成结果为空、画布失去尺寸依据，会引发一连串边界问题。
    if (m_layers.size() <= 1)
        return false;

    m_layers.removeAt(index);
    return true;
}

bool LayerList::move(int from, int to)
{
    if (!isValidIndex(from) || !isValidIndex(to))
        return false;
    if (from == to)
        return true;   // 原地不动，视为成功

    // 关键细节：先取出元素再插入，而不是用 swap。
    // QList::move 的语义正好是"把 from 移到 to"，内部已处理索引偏移，
    // 所以这里直接用，不再自己算 —— 自己算索引是最容易出 off-by-one 的地方。
    m_layers.move(from, to);
    return true;
}

bool LayerList::moveUp(int index)
{
    // "上移"在视觉上意味着往图层栈的**顶部**走，也就是索引增大。
    // 这个反直觉的点必须注意：UI 上的"上"等于索引 + 1。
    if (!isValidIndex(index) || index >= m_layers.size() - 1)
        return false;

    m_layers.move(index, index + 1);
    return true;
}

bool LayerList::moveDown(int index)
{
    if (!isValidIndex(index) || index <= 0)
        return false;

    m_layers.move(index, index - 1);
    return true;
}

QList<Layer> LayerList::cloneAll() const
{
    QList<Layer> copies;
    copies.reserve(m_layers.size());
    for (const Layer &layer : m_layers) {
        // Layer::clone() 内部做了 QImage::copy()，因此这里得到的是独立副本。
        // 撤销快照必须用这个而不是直接赋值整个 QList —— 后者只会拷贝
        // Layer 对象本身，像素数据仍然是共享的。
        copies.append(layer.clone());
    }
    return copies;
}

} // namespace zl
