#pragma once

// ============================================================================
//  ImageMeta.h —— 图像元数据（EXIF / DPI / 自动旋转）
// ----------------------------------------------------------------------------
//  这个文件的实用价值集中在一点：**EXIF 自动旋转**。
//
//  手机拍的照片通常带一个 Orientation 标记，说明"这张图应该转 90° 才正"。
//  相机出于成本考虑把传感器数据原样存下来，只是打了个标记，而不是真的旋转
//  像素。如果打开时不处理这个标记，用户会发现"明明拍的是竖图，
//  打开却是横的躺在那"。
//
//  Qt 5.5 起提供了 QImageReader::transformation()，直接返回需要的变换，
//  实现只有几行代码，而收益非常明显 —— 这是投入产出比最高的一项功能。
//
//  另一个要点是 DPI 元数据：图像的物理分辨率决定打印尺寸。
//  72 DPI 的 1000 像素图打印出来是 35cm，300 DPI 的同一张图是 8.5cm。
//  图像大小对话框里的"文档大小"一栏需要这个数据。
// ============================================================================

#include <QImage>
#include <QSize>
#include <QString>
#include <QImageReader>

namespace zl {

// ---------------------------------------------------------------------------
//  图像元数据
// ---------------------------------------------------------------------------
struct ImageMetadata
{
    QSize   size;                       // 像素尺寸
    qreal   dpiX = 72.0;                // 水平物理分辨率
    qreal   dpiY = 72.0;                // 垂直物理分辨率
    QString formatName;                 // "PNG" / "JPEG" 等
    QString colorSpaceName;             // 通常为 "sRGB"
    bool    hasAlpha = false;           // 是否带 alpha 通道
    bool    hasOrientation = false;     // 是否带 EXIF 方向标记

    // 是否需要自动旋转（依据 EXIF Orientation）
    QImageIOHandler::Transformations orientation =
        QImageIOHandler::TransformationNone;

    // 物理尺寸（英寸）。打印对话框与图像大小对话框会用到。
    QSizeF  physicalSizeInches() const
    {
        const qreal dx = (dpiX > 0.0) ? dpiX : 72.0;
        const qreal dy = (dpiY > 0.0) ? dpiY : 72.0;
        return QSizeF(size.width() / dx, size.height() / dy);
    }

    // 物理尺寸（厘米），更符合国内用户习惯
    QSizeF physicalSizeCentimeters() const
    {
        const QSizeF inches = physicalSizeInches();
        return QSizeF(inches.width() * 2.54, inches.height() * 2.54);
    }
};

// ---------------------------------------------------------------------------
//  读取元数据
// ---------------------------------------------------------------------------

// 只读取头部信息，**不加载像素数据**。
//
//  这个函数存在的意义就是"快"：打开一个 50MB 的 TIFF 时，用户希望立刻
//  看到图像信息（尺寸、格式），而不是等整张图解码完。
//  QImageReader 在读元数据时只解析文件头，通常几毫秒就完成。
//
//  失败时返回的对象 size 为无效值，调用方应检查。
ImageMetadata readMetadata(const QString &filePath);

// 读取元数据并同时应用 EXIF 自动旋转。
//
//  这是打开文件的**主入口**。它做两件事：
//    1. 用 QImageReader 读图（带 setAutoTransform(true)，Qt 会自动处理方向）
//    2. 收集元数据
//
//  关于 setAutoTransform：
//    设为 true 后，QImageReader 在解码时自动应用 EXIF 方向变换，
//    读出来的 QImage 已经是摆正的。这比自己读 transformation() 再手动
//    旋转更省事，也不容易出错。
//
//    但要注意：**这会改变图像的像素尺寸**（竖图变成 4000×3000 而非
//    3000×4000），因此元数据里的 size 必须以旋转后的为准。
//
//  scaleToSize 非空时，用 QImageReader 的缩略图解码能力做高效缩放 ——
//  JPEG 支持 DCT 缩放，解码 1/2 或 1/4 尺寸比"全解码再缩放"快得多。
QImage loadImageWithMetadata(const QString &filePath,
                             ImageMetadata *metadataOut = nullptr,
                             const QSize &scaleToSize = QSize());

// ---------------------------------------------------------------------------
//  DPI 读写
// ---------------------------------------------------------------------------

// 从 QImage 读取 DPI（QImage 自身携带 dotsPerMeterX/Y）
void readDpiFromImage(const QImage &image, qreal &dpiX, qreal &dpiY);

// 把 DPI 写入 QImage（会同时设置 dotsPerMeterX/Y，两者是同一数据的
// 不同单位表示，必须一起设，否则某些程序读到的不一致）
void writeDpiToImage(QImage &image, qreal dpiX, qreal dpiY);

// ---------------------------------------------------------------------------
//  格式能力查询
// ---------------------------------------------------------------------------

// 某个格式是否支持 alpha 通道。导出对话框需要据此决定"透明背景"选项
// 是否可用 —— JPEG 不支持 alpha，选了透明背景只能被填充成白色。
bool formatSupportsAlpha(const QByteArray &format);

// 某个格式是否支持有损质量参数
bool formatSupportsQuality(const QByteArray &format);

// 某个格式是否支持多帧（动画 GIF）
bool formatSupportsAnimation(const QByteArray &format);

// 列出所有可写格式的文件名过滤器，用于另存为对话框。
// 返回形如 "PNG 图像 (*.png)" 的字符串列表。
QStringList writableFormatFilters();

// ---------------------------------------------------------------------------
//  文件大小预估
// ---------------------------------------------------------------------------

// 估算以指定格式与质量保存后的大致字节数。
//
//  实现方式：对图像做一个小的降采样副本，用相同的参数编码，
//  再按面积比例放大结果。这是"估算"，因为压缩率与图像内容强相关 ——
//  但误差通常在 ±20% 以内，足够给用户一个数量级上的预期
//  （"保存出来大概是 2MB 还是 20MB"）。
//
//  完整编码一遍再取大小也是可行的，但 4000×3000 的 JPEG 编码要 100ms 以上，
//  拖动质量滑块时每次都要等，交互会非常卡。
qint64 estimateFileSize(const QImage &image, const QByteArray &format, int quality);

} // namespace zl
