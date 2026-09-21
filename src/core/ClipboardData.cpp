#include "core/ClipboardData.h"

#include <QBuffer>
#include <QDataStream>

namespace zl {

const char *const kClipboardMimeLayer = "application/x-zlphotoedit-layer";

// ---------------------------------------------------------------------------
//  序列化格式版本
// ---------------------------------------------------------------------------
//  这个数字决定了将来能否安全地演进格式。读取时若发现版本高于当前支持的
//  最高版本，应拒绝解析并给出友好提示，而不是硬着头皮读出一堆乱码。
//  这是从第一天就要养成的习惯 —— 后悔药只能提前吃。
static constexpr quint32 kClipboardFormatVersion = 1;

// 魔数：用于快速判断"这确实是我们的数据"，而不是凑巧 MIME 头对上了
static constexpr quint32 kClipboardMagic = 0x5A4C5048;   // 'ZLPH'

QByteArray ClipboardData::serialize() const
{
    QByteArray result;
    QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly))
        return QByteArray();

    QDataStream out(&buffer);
    // 显式指定版本，保证不同 Qt 版本间的字节序与类型大小一致。
    // 不指定的话，Qt5 与 Qt6 之间交换剪贴板数据会读出一堆垃圾。
    out.setVersion(QDataStream::Qt_5_15);
    out.setByteOrder(QDataStream::LittleEndian);

    out << kClipboardMagic;
    out << kClipboardFormatVersion;

    // 位置信息
    out << qint32(sourceBounds.x())
        << qint32(sourceBounds.y())
        << qint32(sourceBounds.width())
        << qint32(sourceBounds.height());
    out << quint64(sourceDocumentId);

    // 像素：用 PNG 压缩后写入。注意必须显式指定格式 ——
    // 预乘格式的 PNG 往返会丢精度，这里统一按非预乘存储，
    // 读回来时再转回预乘，避免多次复制粘贴后逐渐色偏。
    {
        QImage nonPremul = pixels.format() == QImage::Format_ARGB32_Premultiplied
                               ? pixels.convertToFormat(QImage::Format_ARGB32)
                               : pixels;
        QByteArray pngBytes;
        QBuffer pngBuffer(&pngBytes);
        pngBuffer.open(QIODevice::WriteOnly);
        nonPremul.save(&pngBuffer, "PNG");
        pngBuffer.close();

        out << pngBytes;
    }

    // 蒙版（可选）
    {
        QByteArray maskBytes;
        if (!mask.isNull()) {
            QBuffer maskBuffer(&maskBytes);
            maskBuffer.open(QIODevice::WriteOnly);
            mask.save(&maskBuffer, "PNG");
            maskBuffer.close();
        }
        out << maskBytes;
    }

    buffer.close();
    return result;
}

ClipboardData ClipboardData::deserialize(const QByteArray &data)
{
    ClipboardData result;
    if (data.isEmpty())
        return result;

    QBuffer buffer(const_cast<QByteArray *>(&data));
    if (!buffer.open(QIODevice::ReadOnly))
        return result;

    QDataStream in(&buffer);
    in.setVersion(QDataStream::Qt_5_15);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    quint32 version = 0;
    in >> magic >> version;

    // 魔数或版本不对，说明不是我们的数据（或来自未来版本）。
    // 直接返回无效对象，让调用方退回到读取 image/png 的路径。
    // 这里绝不抛异常、绝不崩溃 —— 剪贴板内容是不可信的外部输入。
    if (magic != kClipboardMagic || version > kClipboardFormatVersion)
        return result;

    qint32 x = 0, y = 0, w = 0, h = 0;
    quint64 docId = 0;
    in >> x >> y >> w >> h;
    in >> docId;

    result.sourceBounds = QRect(x, y, w, h);
    result.sourceDocumentId = quintptr(docId);

    QByteArray pngBytes;
    in >> pngBytes;
    if (!pngBytes.isEmpty()) {
        QImage img;
        if (!img.loadFromData(pngBytes, "PNG"))
            return ClipboardData();     // 数据损坏，视为无效

        // 转回预乘格式。图层位图的铁律要求所有像素数据都是预乘格式，
        // 在剪贴板这个"入口"处统一转换，后续代码就无需再关心。
        result.pixels = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QByteArray maskBytes;
    in >> maskBytes;
    if (!maskBytes.isEmpty()) {
        QImage m;
        if (m.loadFromData(maskBytes, "PNG"))
            result.mask = m.convertToFormat(QImage::Format_Grayscale8);
    }

    buffer.close();
    return result;
}

} // namespace zl
