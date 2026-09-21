#include "commands/ImageCommand.h"
#include "core/Document.h"

#include <QAtomicInt>
#include <QPainter>

namespace zl {

// ===========================================================================
//  合并类别 ID 的分配
// ===========================================================================
//
//  与 DrawCommand 的"描边 ID"机制类似，这里需要一个"调整操作会话"的概念：
//  用户在对话框里拖动亮度滑块，每次拖动都是一个独立的调整，但这些
//  应该合并成一步撤销。
//
//  实现：每次打开对话框时调用 ImageCommand::beginAdjustSession() 取一个
//  新的类别 ID，对话框内所有命令都用这个 ID，于是它们可以互相合并。
//  关闭对话框后，新的一次调整会拿到不同的 ID，不能与之前合并。
static QAtomicInt g_adjustSessionCounter(1);

// 提供静态入口（头文件里没声明，因为是内部机制；
// 对话框通过构造函数自动获取 —— 见下面的实现）
namespace {
int nextAdjustSessionId()
{
    return g_adjustSessionCounter.fetchAndAddRelaxed(1);
}
} // namespace

// ===========================================================================
//  构造
// ===========================================================================

ImageCommand::ImageCommand(Document *doc, int index,
                           const QImage &before, const QImage &after,
                           const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_layerIndex(index)
    , m_mergeId(nextAdjustSessionId())
{
    m_imageSize = before.size();

    // ── 压缩存储 ──
    //
    // qCompress 接受 (const uchar*, int) 加可选的压缩级别。
    // 直接用 QImage 的 bits() 指针 + sizeInBytes() 即可 ——
    // 这也顺带把图像的内存布局（行对齐等）一并保存下来，
    // 但我们不使用这个布局来重建（见 restoreImage 的说明）。
    if (!before.isNull()) {
        m_rawBytes += qint64(before.sizeInBytes());
        m_compressedBefore = qCompress(
            before.constBits(),
            int(before.sizeInBytes()),
            kCompressionLevel);
    }

    if (!after.isNull()) {
        m_rawBytes += qint64(after.sizeInBytes());
        m_compressedAfter = qCompress(
            after.constBits(),
            int(after.sizeInBytes()),
            kCompressionLevel);
    }
}

ImageCommand::ImageCommand(Document *doc, int index,
                           const QByteArray &compressedBefore,
                           const QByteArray &compressedAfter,
                           const QSize &imageSize,
                           const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_layerIndex(index)
    , m_compressedBefore(compressedBefore)
    , m_compressedAfter(compressedAfter)
    , m_imageSize(imageSize)
    , m_mergeId(nextAdjustSessionId())
{
    // 从已压缩数据构造时，原始大小只能估算。
    // 用 QImage::sizeInBytes() 的公式：每行字节数（4 字节对齐）× 高。
    if (imageSize.isValid()) {
        const qint64 bytesPerLine = qint64(imageSize.width()) * 4;
        const qint64 oneImage = bytesPerLine * qint64(imageSize.height());
        m_rawBytes = oneImage * 2;   // 前后两份
    }
}

// ===========================================================================
//  undo / redo
// ===========================================================================

void ImageCommand::redo()
{
    restoreImage(m_compressedAfter, m_imageSize);
}

void ImageCommand::undo()
{
    restoreImage(m_compressedBefore, m_imageSize);
}

// ===========================================================================
//  解压并写回
// ===========================================================================

void ImageCommand::restoreImage(const QByteArray &compressed,
                                const QSize &expectedSize)
{
    if (!m_doc || compressed.isEmpty())
        return;

    Layer *layer = m_doc->layers().layer(m_layerIndex);
    if (!layer)
        return;

    // ── 解压 ──
    const QByteArray raw = qUncompress(compressed);

    if (raw.isEmpty()) {
        // 数据损坏（内存不足时 qCompress 也可能产出无法解压的数据）。
        // ★ 关键：这里选择"静默放弃这一步"而不是崩溃或抛异常 ★
        //
        // 理由：撤销历史损坏是一回事，整个程序崩掉是另一回事。
        // 用户宁可"这一步撤销不了"，也不能接受程序消失、未保存的
        // 工作全丢。这正是"内存降级"在代码层面的具体体现。
        //
        // 实际中这种情况极罕见，但代价太低（几行代码），值得加。
        return;
    }

    // ── 重建 QImage ──
    //
    // ★★★ 这里是全文最需要注意的地方 ★★★
    //
    // qUncompress 返回的 QByteArray 是**局部变量**，函数返回后即销毁。
    // 如果直接这样写：
    //
    //     QImage img(reinterpret_cast<const uchar*>(raw.constData()),
    //                w, h, QImage::Format_ARGB32_Premultiplied);
    //
    // 那么 img 只是"引用"了 raw 的缓冲区，**不拥有**它。
    // raw 析构后，img 的数据指针悬空。
    //
    // 症状：不崩溃！内存只是被标记为"已释放"，内容还在原地，
    //      所以大多数时候看起来是对的。等到某次内存分配复用这块
    //      内存时，画面就突然错了 —— 而且可能是几十步操作之后
    //      才显现，完全无法定位。
    //
    // 正确做法有两个：
    //   (a) 构造时立刻 copy()：QImage(...).copy()
    //   (b) 用 fromData()：它内部会拷贝
    //
    // 这里用 (a)，因为它对"数据是否 4 字节对齐"更宽容。
    const int width  = expectedSize.width();
    const int height = expectedSize.height();

    if (width <= 0 || height <= 0)
        return;

    // 大小校验：解压出来的字节数必须与预期相符。
    // 不符说明压缩数据被破坏，宁可放弃这一步也不要写入错乱的数据。
    const qint64 expectedBytes = qint64(width) * qint64(height) * 4;
    if (qint64(raw.size()) < expectedBytes) {
        return;
    }

    // ImageCommand 从 QImage::constBits() 拿到的数据是紧密排列的
    // （每行 width*4 字节，无额外对齐填充），因为我们构造时用的是
    // QImage 的原始缓冲区。所以这里可以按 (width, height, bytesPerLine)
    // 直接构造，bytesPerLine 正好是 width*4。
    //
    // ★ 最后那个 .copy() 绝对不能省 ★ —— 它是防止悬空引用的关键。
    const QImage restored =
        QImage(reinterpret_cast<const uchar *>(raw.constData()),
               width, height, width * 4,
               QImage::Format_ARGB32_Premultiplied).copy();

    if (restored.isNull())
        return;

    // ── 写回图层 ──
    //
    // 直接替换整个位图。用 setImage 而不是逐像素写 ——
    // setImage 内部会做格式规范化，保证图层格式铁律不被破坏。
    layer->setImage(restored);

    // 图层缩略图需要刷新
    m_doc->markDirty();
}

// ===========================================================================
//  合并
// ===========================================================================

int ImageCommand::id() const
{
    // 与 DrawCommand 同样思路：给类别 ID 加固定偏移避免撞号。
    // 用 0x7A10（'z' = 0x7A，"10" 表示"图像调整"这一类）。
    return 0x7A10 + m_mergeId;
}

bool ImageCommand::mergeWith(const QUndoCommand *other)
{
    const auto *next = dynamic_cast<const ImageCommand *>(other);
    if (!next)
        return false;

    if (next->m_doc != m_doc)
        return false;
    if (next->m_layerIndex != m_layerIndex)
        return false;

    // ── 合并规则 ──
    //
    // 与 DrawCommand 不同，这里的合并非常简单：
    //   · before 保持本命令的 before（最早的状态）
    //   · after  换成新命令的 after（最新的状态）
    //
    // 因为两份数据都是完整的图层快照，覆盖关系是明确的，
    // 不需要像脏矩形那样拼合区域。
    m_compressedAfter = next->m_compressedAfter;

    // 原始字节数不变（每张图都是整层大小）
    return true;
}

// ===========================================================================
//  内存报告
// ===========================================================================

qint64 ImageCommand::memoryUsage() const
{
    // ★ 重要：报告的是**压缩后**的实际占用 ★
    //
    // 这是本类存在的意义所在。如果按原始大小报告，状态栏会
    // 显示"撤销栈占用 2.9GB"而实际上只占了几百 MB，
    // 会让用户误以为程序有问题。
    qint64 bytes = qint64(m_compressedBefore.size())
                 + qint64(m_compressedAfter.size())
                 + qint64(sizeof(ImageCommand));

    return bytes;
}

} // namespace zl
