#include "engine/IcoWriter.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QSaveFile>
#include <QtGlobal>

namespace zl {
namespace ico {
namespace {

// ---------------------------------------------------------------------------
//  小端写入
// ---------------------------------------------------------------------------
//  ICO 是 Windows 格式，**全部字段都是小端**。不借助 QDataStream 而手写这三个
//  函数，是为了让"哪个字段几个字节、什么字节序"在调用处一眼可见 ——
//  这个文件的价值就在于字节级结构，藏起来反而不好读。
//
//  ★ 不用 QDataStream 的另一个理由 ★
//    QDataStream 默认大端，必须记得 setByteOrder()；漏设的话产出的文件
//    "看着完全正常"（能写能读），但任何图标程序都打不开。
//    手写 putU16/putU32 没有"忘记设"这个失败模式。
void putU8(QByteArray &out, quint8 v)
{
    out.append(char(v));
}

void putU16(QByteArray &out, quint16 v)
{
    out.append(char(quint8(v & 0xFF)));
    out.append(char(quint8((v >> 8) & 0xFF)));
}

void putU32(QByteArray &out, quint32 v)
{
    out.append(char(quint8(v & 0xFF)));
    out.append(char(quint8((v >> 8) & 0xFF)));
    out.append(char(quint8((v >> 16) & 0xFF)));
    out.append(char(quint8((v >> 24) & 0xFF)));
}

// 目录项里的宽/高字段：1 字节，**0 表示 256**。
//
//  写不下更大的值（这正是 256 以上必须改走 PNG 条目的原因）。
//  对 PNG 条目来说这个字节只是个"提示"，真实尺寸以 PNG 头为准，写 0 是通行做法。
quint8 dimByte(int edge)
{
    return quint8(edge >= 256 ? 0 : edge);
}

// 固定头 + 一条目录项的字节数。
constexpr int kIconDirBytes     = 6;
constexpr int kIconDirEntryBytes = 16;
constexpr int kBmpInfoHdrBytes  = 40;

// 图像数据的起始偏移。本实现只写一条图标，所以是常量。
constexpr quint32 kFirstImageOffset = quint32(kIconDirBytes + kIconDirEntryBytes);

void setError(QString *errorOut, const QString &text)
{
    if (errorOut)
        *errorOut = text;
}

// ---------------------------------------------------------------------------
//  BMP 条目的图像数据
// ---------------------------------------------------------------------------
//  40 字节头 + 自下而上的 32bpp BGRA + 自下而上的 1bpp AND 掩码。
//
//  ★★ 两个"自下而上"和一个是两倍的高度 ★★
//    DIB 的历史包袱：位图行序是**从下往上**，而 ICO 的头里
//    biHeight 记的是 **XOR 图 + AND 掩码的合计高度**（即 2×h）。
//    漏掉 ×2 的话，读图程序会把像素数据的前一半当成图像、后一半当成掩码，
//    症状是"图标上下错位、下半张变成黑白噪点"。
QByteArray encodeBmpEntry(const QImage &src)
{
    const int w = src.width();
    const int h = src.height();

    const int colorBpl = w * 4;                 // 32bpp：w*4 天然是 4 的倍数
    const int maskBpl  = ((w + 31) / 32) * 4;   // 1bpp：每行补齐到 4 字节

    QByteArray out;
    out.reserve(kBmpInfoHdrBytes + colorBpl * h + maskBpl * h);

    // ── BITMAPINFOHEADER ──
    putU32(out, kBmpInfoHdrBytes);
    putU32(out, quint32(w));
    putU32(out, quint32(h * 2));                // ★ 合计高度（XOR + AND）
    putU16(out, 1);                             // biPlanes
    putU16(out, 32);                            // biBitCount
    putU32(out, 0);                             // biCompression = BI_RGB
    putU32(out, quint32(colorBpl * h + maskBpl * h));   // biSizeImage
    putU32(out, 0);                             // biXPelsPerMeter
    putU32(out, 0);                             // biYPelsPerMeter
    putU32(out, 0);                             // biClrUsed
    putU32(out, 0);                             // biClrImportant

    // ── XOR 图：自下而上，每像素 B G R A ──
    //
    //  ★ 必须逐通道写，不能直接 memcpy 整行 ★
    //    QImage 的 ARGB32 在小端机器内存里就是 B,G,R,A —— 顺手上看着一样，
    //    但"内存布局"是平台约定，而"文件字节序"是格式规定。
    //    写明 qBlue/qGreen/qRed/qAlpha 之后，大端机器上也是对的。
    for (int y = h - 1; y >= 0; --y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb p = row[x];
            putU8(out, quint8(qBlue(p)));
            putU8(out, quint8(qGreen(p)));
            putU8(out, quint8(qRed(p)));
            putU8(out, quint8(qAlpha(p)));
        }
    }

    // ── AND 掩码：自下而上，**置 1 = 全透明** ──
    //
    //  ★ 为什么自己按位拼，而不是 QImage::Format_Mono + createAlphaMask ★
    //    createAlphaMask 的阈值语义与"是否要盖住背景"并不等价，
    //    而且 Format_Mono 有颜色表、invertPixels() 在 1bpp 上改的是
    //    "颜色表"还是"位"在不同 Qt 版本口径不一 —— 一个字节一个字节写
    //    反而最短、最没有歧义。
    //
    //  ★ 位序：最高位 = 最左边的像素 ★（DIB 的约定）
    for (int y = h - 1; y >= 0; --y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const int at = out.size();
        out.append(maskBpl, '\0');
        char *bits = out.data() + at;
        for (int x = 0; x < w; ++x) {
            if (qAlpha(row[x]) == 0) {
                const int byteIdx = x >> 3;
                const quint8 mask  = quint8(0x80 >> (x & 7));
                bits[byteIdx] = char(quint8(bits[byteIdx]) | mask);
            }
        }
    }

    return out;
}

} // namespace

// ===========================================================================
//  尺寸判定
// ===========================================================================

bool canEncode(const QSize &size)
{
    if (!size.isValid())
        return false;
    return size.width()  > 0 && size.height() > 0
        && size.width()  <= kMaxIconEdge
        && size.height() <= kMaxIconEdge;
}

QString rejectionMessage(const QSize &size)
{
    if (canEncode(size))
        return QString();

    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        // 这些消息最终由 UI 层显示，所以走 translate（context 显式给出，
        // 与匿名 namespace 里的 tr() 对不上的问题同理，见
        // docs/界面语言.md §4.3；字面量直传，lupdate 才提取）。
        return QCoreApplication::translate("zl::ico",
                   "The image size is invalid; cannot export as ICO.");
    }

    // ★ 上限值只在这一处以 QString::number 出现，界面层不重复写死 ★
    return QCoreApplication::translate("zl::ico",
               "The image size %1 × %2 exceeds the ICO format limit of %3 × %3.\n\n"
               "ICO is an icon format with a maximum edge of %3 pixels "
               "(Windows officially says 256; this program allows %3).\n\n"
               "Use Image → Image Size to reduce the image to %3 pixels or "
               "less, then save as ICO.")
        .arg(size.width())
        .arg(size.height())
        .arg(kMaxIconEdge);
}

EntryFormat entryFormatFor(const QSize &size)
{
    // 宽或高**任一**超过 256 就得换 PNG 条目 ——
    // 目录项里两个字段各只有 1 字节，表达不了 256 以上。
    if (size.width() > kBmpEntryMaxEdge || size.height() > kBmpEntryMaxEdge)
        return EntryFormat::Png;
    return EntryFormat::Bmp;
}

// ===========================================================================
//  编码
// ===========================================================================

QByteArray encode(const QImage &image, QString *errorOut)
{
    if (image.isNull()) {
        setError(errorOut, QStringLiteral("图像为空，无法导出为 ICO。"));
        return QByteArray();
    }

    const QSize size = image.size();
    if (!canEncode(size)) {
        setError(errorOut, rejectionMessage(size));
        return QByteArray();
    }

    // ★ 统一成**非预乘** ARGB32 ★
    //   本工程内部一律 ARGB32_Premultiplied。预乘数据直接取 qRed()/qBlue()
    //   拿到的是"已经乘过 alpha"的值，写进文件后半透明边缘会明显偏暗 ——
    //   与 MainWindow::writeImage 里那条注释是同一个坑。
    //   不透明像素完全正常，所以只有带透明边缘的图才会暴露。
    const QImage src = (image.format() == QImage::Format_ARGB32)
                           ? image
                           : image.convertToFormat(QImage::Format_ARGB32);
    if (src.isNull()) {
        setError(errorOut, QStringLiteral("无法转换图像格式，导出 ICO 失败。"));
        return QByteArray();
    }

    const int w = size.width();
    const int h = size.height();
    const EntryFormat entryFmt = entryFormatFor(size);

    // ── 先准备图像数据 ──
    QByteArray payload;
    if (entryFmt == EntryFormat::Png) {
        QBuffer buffer(&payload);
        if (!buffer.open(QIODevice::WriteOnly)) {
            setError(errorOut, QStringLiteral("无法创建编码缓冲区，导出 ICO 失败。"));
            return QByteArray();
        }
        // PNG 是 QtGui 内置格式（不是插件），不依赖任何 .dll
        if (!src.save(&buffer, "PNG")) {
            setError(errorOut, QStringLiteral("PNG 编码失败，导出 ICO 失败。"));
            return QByteArray();
        }
        buffer.close();
        if (payload.isEmpty()) {
            setError(errorOut, QStringLiteral("PNG 编码结果为空，导出 ICO 失败。"));
            return QByteArray();
        }
    } else {
        payload = encodeBmpEntry(src);
    }

    // ── 拼装文件 ──
    QByteArray out;
    out.reserve(kIconDirBytes + kIconDirEntryBytes + payload.size());

    // ICONDIR
    putU16(out, 0);            // idReserved，必须是 0
    putU16(out, 1);            // idType：1 = 图标（2 是光标）
    putU16(out, 1);            // idCount：只写一条

    // ICONDIRENTRY
    putU8(out,  dimByte(w));
    putU8(out,  dimByte(h));
    putU8(out,  0);            // bColorCount：≥8bpp 时固定 0
    putU8(out,  0);            // bReserved
    putU16(out, 1);            // wPlanes
    putU16(out, 32);           // wBitCount（PNG 条目这里同样标 32bpp）
    putU32(out, quint32(payload.size()));
    putU32(out, kFirstImageOffset);

    out.append(payload);
    return out;
}

bool write(const QString &path, const QImage &image, QString *errorOut)
{
    if (path.isEmpty()) {
        setError(errorOut, QStringLiteral("未指定输出路径。"));
        return false;
    }

    const QByteArray bytes = encode(image, errorOut);
    if (bytes.isEmpty())
        return false;

    // ★ 用 QSaveFile 而不是 QFile ★
    //   写一半失败（磁盘满 / 被占用）时不会留下一个"看着像图标、
    //   其实截断了"的半成品文件 —— 那种文件双击打不开，用户会以为
    //   程序把原图弄坏了。QSaveFile 走临时文件 + 原子改名。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorOut,
                 QStringLiteral("无法写入文件：%1").arg(file.errorString()));
        return false;
    }

    if (file.write(bytes) != bytes.size()) {
        setError(errorOut,
                 QStringLiteral("写入不完整：%1").arg(file.errorString()));
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        setError(errorOut,
                 QStringLiteral("保存失败：%1").arg(file.errorString()));
        return false;
    }

    return true;
}

} // namespace ico
} // namespace zl
