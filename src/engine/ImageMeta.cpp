#include "engine/ImageMeta.h"
#include "engine/Resample.h"

#include <QImageWriter>
#include <QBuffer>
#include <QFileInfo>
#include <QTextStream>
#include <QColorSpace>
#include <QPainter>
#include <QtMath>

namespace zl {

// ===========================================================================
//  读取元数据
// ===========================================================================

ImageMetadata readMetadata(const QString &filePath)
{
    ImageMetadata meta;

    if (filePath.isEmpty())
        return meta;

    QImageReader reader(filePath);

    // 只是读头部，这里不做任何解码。QImageReader 的 size() 与 format()
    // 都只依赖文件头，因此即使文件有几百 MB 也是毫秒级返回。
    if (!reader.canRead()) {
        // 无法读取：可能是文件损坏、格式不支持、或没有读权限。
        // 这里不抛异常也不崩溃，返回一个 size 无效的对象让调用方判断。
        return meta;
    }

    meta.size = reader.size();
    meta.formatName = QString::fromUtf8(reader.format()).toUpper();
    meta.orientation = reader.transformation();
    meta.hasOrientation = (meta.orientation != QImageIOHandler::TransformationNone);

    // ── EXIF 旋转对元数据尺寸的影响 ──
    //
    // 这一点很容易漏：如果照片带了 90°/270° 的方向标记，加载时 Qt 会自动
    // 旋转，那么**实际得到的图像宽高是互换的**。
    // 如果元数据里报的还是未旋转的尺寸，打开对话框就会显示错误的宽高，
    // 而且后续按"图像大小"做的计算会全错。
    //
    // 这里提前把尺寸换过来，保证元数据与将要加载的图像一致。
    const bool rotated90 =
        (meta.orientation & QImageIOHandler::TransformationRotate90) != 0;
    if (rotated90 && meta.size.isValid())
        meta.size.transpose();

    // 格式决定是否可能有 alpha
    meta.hasAlpha = (meta.formatName == QStringLiteral("PNG")
                  || meta.formatName == QStringLiteral("TGA")
                  || meta.formatName == QStringLiteral("TIFF")
                  || meta.formatName == QStringLiteral("WEBP")
                  || meta.formatName == QStringLiteral("GIF")
                  || meta.formatName == QStringLiteral("ICO")
                  || meta.formatName == QStringLiteral("BMP"));

    // DPI 需要真正解码才能读到（存在图像自身的 dotsPerMeter 属性里）。
    // 为了不破坏"只读头部"这个性能承诺，这里默认给 72，由
    // loadImageWithMetadata() 在加载后再更新一次。
    meta.dpiX = 72.0;
    meta.dpiY = 72.0;
    meta.colorSpaceName = QStringLiteral("sRGB");

    return meta;
}

QImage loadImageWithMetadata(const QString &filePath,
                             ImageMetadata *metadataOut,
                             const QSize &scaleToSize)
{
    if (filePath.isEmpty())
        return QImage();

    QImageReader reader(filePath);

    if (!reader.canRead())
        return QImage();

    // ── 启用 EXIF 自动方向 ──
    //
    // 设为 true 后 Qt 在解码时自动应用 EXIF 的 Orientation 标记，
    // 读出来的图已经是正的了。这是打开手机照片必须的一步，
    // 否则用户会看到"竖拍的照片横躺着"。
    reader.setAutoTransform(true);

    // ── 高效缩放 ──
    //
    // 如果只要一个缩略图（或者图像大小对话框的预览），
    // 用 QImageReader::setScaledSize 让解码器直接输出小尺寸。
    // JPEG 的解码器支持 DCT 缩放（按 1/2、1/4、1/8 直接跳过高频系数），
    // 这比"全尺寸解码再缩放"快好几倍，内存占用也低得多 ——
    // 打开 6000×4000 的照片做缩略图时，这个差别是"瞬间"和"等一秒"。
    if (scaleToSize.isValid() && !scaleToSize.isEmpty()) {
        const QSize originalSize = reader.size();
        if (originalSize.isValid() && !originalSize.isEmpty()) {
            // 保持长宽比地缩放到目标尺寸内
            const QSize scaled = originalSize.scaled(scaleToSize, Qt::KeepAspectRatio);
            reader.setScaledSize(scaled);
        }
    }

    QImage image = reader.read();

    if (image.isNull())
        return QImage();

    // ── 回填元数据 ──
    if (metadataOut) {
        ImageMetadata meta;

        // 若有缩放，size 以实际加载结果为准（缩略图的尺寸）
        meta.size = image.size();
        meta.formatName = QString::fromUtf8(reader.format()).toUpper();
        meta.orientation = reader.transformation();
        meta.hasOrientation = (meta.orientation != QImageIOHandler::TransformationNone);
        meta.hasAlpha = image.hasAlphaChannel();

        // DPI 从图像自身属性读取
        readDpiFromImage(image, meta.dpiX, meta.dpiY);

        // ── 色彩空间描述 ──
        //
        // ★ Qt5 与 Qt6 的 QColorSpace 都没有一个"一定能拿到名字"的接口 ★
        //   QColorSpace::name() 是 Qt 6.0 才加入的；
        //   而 description() 是 Qt 5.14 的 QColorSpace 私有接口，5.15 起
        //   被标记废弃、6.0 被删除 —— 两条路都走不通。
        //
        //   可行的接口是 QColorSpace 的 operator== 与 QColorSpace::NamedColorSpace
        //   枚举。这里用"与几个内置标准色彩空间比较"的方式反推名字 ——
        //   覆盖了实际会遇到的全部情况，且两版 Qt 都支持。
        //
        //   本字段只用于信息展示，不参与任何像素运算
        //   （本工程明确不做色彩管理），所以这个近似足够。
        const QColorSpace cs = image.colorSpace();

        if (!cs.isValid()) {
            meta.colorSpaceName = QStringLiteral("sRGB");
        } else if (cs == QColorSpace(QColorSpace::SRgb)) {
            meta.colorSpaceName = QStringLiteral("sRGB");
        } else if (cs == QColorSpace(QColorSpace::SRgbLinear)) {
            meta.colorSpaceName = QStringLiteral("sRGB Linear");
        } else if (cs == QColorSpace(QColorSpace::AdobeRgb)) {
            meta.colorSpaceName = QStringLiteral("Adobe RGB");
        } else if (cs == QColorSpace(QColorSpace::DisplayP3)) {
            meta.colorSpaceName = QStringLiteral("Display P3");
        } else if (cs == QColorSpace(QColorSpace::ProPhotoRgb)) {
            meta.colorSpaceName = QStringLiteral("ProPhoto RGB");
        } else {
            // 自定义/ICC 配置文件带来的色彩空间。
            // 不谎报一个具体的名字（那会误导用户），只说明它有色彩空间信息。
            meta.colorSpaceName = QStringLiteral("自定义（含 ICC 配置文件）");
        }

        *metadataOut = meta;
    }

    return image;
}

// ===========================================================================
//  DPI
// ===========================================================================

void readDpiFromImage(const QImage &image, qreal &dpiX, qreal &dpiY)
{
    // QImage 用"每米点数"存储物理分辨率，需要换算成每英寸点数。
    // 1 英寸 = 0.0254 米，所以 DPI = dotsPerMeter * 0.0254。
    //
    // 大部分图像没有这个信息时，QImage 会给 0 或 3780（96 DPI 对应的
    // 每米点数）。数值为 0 时应当回退到 72 DPI 而不是返回 0 ——
    // 否则"物理尺寸"会算出除以零。
    const qreal dpcmX = image.dotsPerMeterX();
    const qreal dpcmY = image.dotsPerMeterY();

    dpiX = (dpcmX > 1.0) ? dpcmX * 0.0254 : 72.0;
    dpiY = (dpcmY > 1.0) ? dpcmY * 0.0254 : 72.0;

    // 合理性校验：DPI 在 [1, 10000] 之外基本可以确定是坏数据。
    // 有些相机厂商会把 0xFFFFFFFF 之类的哨兵值写进这个字段，
    // 换算出来是个天文数字，直接用于计算会产生荒谬的打印尺寸。
    if (dpiX < 1.0 || dpiX > 10000.0)
        dpiX = 72.0;
    if (dpiY < 1.0 || dpiY > 10000.0)
        dpiY = 72.0;
}

void writeDpiToImage(QImage &image, qreal dpiX, qreal dpiY)
{
    if (image.isNull())
        return;

    // 两个单位字段必须同时设置。
    // 只设 dotsPerMeter 而不管 dotPerMeterX/Y 的对应关系，
    // 会导致不同程序读到的 DPI 不一致（有的读 X/Y 字段，有的读每米字段）。
    const qreal dpiXSafe = qBound(1.0, dpiX, 10000.0);
    const qreal dpiYSafe = qBound(1.0, dpiY, 10000.0);

    image.setDotsPerMeterX(qRound(dpiXSafe / 0.0254));
    image.setDotsPerMeterY(qRound(dpiYSafe / 0.0254));
}

// ===========================================================================
//  格式能力
// ===========================================================================

bool formatSupportsAlpha(const QByteArray &format)
{
    const QByteArray f = format.toLower();

    // JPEG 与 BMP 不支持 alpha。
    // BMP 在技术上有一个 alpha 掩码扩展，但支持极不一致，实际导出时
    // 透明区域会变成黑色，所以按"不支持"处理更诚实。
    if (f == "jpg" || f == "jpeg" || f == "bmp" || f == "ppm" || f == "pbm")
        return false;

    // 其余常见格式都支持（PNG/TIFF/WEBP/TGA/GIF/ICO）
    return true;
}

bool formatSupportsQuality(const QByteArray &format)
{
    const QByteArray f = format.toLower();

    // 只有有损格式才有质量参数
    return f == "jpg" || f == "jpeg" || f == "webp";
}

bool formatSupportsAnimation(const QByteArray &format)
{
    const QByteArray f = format.toLower();
    return f == "gif" || f == "webp";
}

QStringList writableFormatFilters()
{
    QStringList filters;

    // 顺序按日常使用频率排列，PNG 与 JPEG 放最前面
    filters << QObject::tr("PNG Images (*.png)")
            << QObject::tr("JPEG Images (*.jpg *.jpeg)")
            << QObject::tr("WebP Images (*.webp)")
            << QObject::tr("BMP Images (*.bmp)")
            << QObject::tr("TIFF Images (*.tif *.tiff)")
            << QObject::tr("TGA Images (*.tga)")
            << QObject::tr("ICO Icons (*.ico)");

    return filters;
}

// ===========================================================================
//  文件大小预估
// ===========================================================================

qint64 estimateFileSize(const QImage &image, const QByteArray &format, int quality)
{
    if (image.isNull() || format.isEmpty())
        return 0;

    // ── 采样编码 ──
    //
    // 不做全尺寸编码（4000×3000 的 JPEG 要 100ms 以上，拖动质量滑块会卡），
    // 而是把图缩小到约 512 像素再编码，然后按**面积比**放大结果。
    //
    // 精度说明：压缩算法的开销有一部分与内容无关（文件头、色度表等），
    // 因此小样本会略微高估——这部分在代码里用一个小的修正量补偿。
    const int sampleEdge = 512;

    QImage sample = image;
    const bool needScale = image.width() > sampleEdge || image.height() > sampleEdge;
    if (needScale) {
        // 估算场景对画质不敏感，用双线性就够，而且快
        sample = resample::scale(image, image.size().scaled(sampleEdge, sampleEdge,
                                                            Qt::KeepAspectRatio),
                                 ResampleMethod::Bilinear);
    }

    if (sample.isNull())
        return 0;

    QByteArray encoded;
    {
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly))
            return 0;

        QImageWriter writer(&buffer, format);
        if (formatSupportsQuality(format))
            writer.setQuality(qBound(1, quality, 100));

        // JPEG 不支持 alpha，先把透明区域压到白色背景上。
        // 否则 Qt 会把 alpha 丢弃，透明区域变成黑色，压缩率与最终结果不符。
        QImage toWrite = sample;
        if (!formatSupportsAlpha(format) && sample.hasAlphaChannel()) {
            QImage flattened(sample.size(), QImage::Format_RGB32);
            flattened.fill(Qt::white);
            QPainter p(&flattened);
            p.drawImage(0, 0, sample);
            p.end();
            toWrite = flattened;
        }

        if (!writer.write(toWrite))
            return 0;
    }

    const qint64 sampleBytes = encoded.size();
    if (sampleBytes <= 0)
        return 0;

    // ── 按面积比放大 ──
    const qreal sampleArea = qreal(sample.width()) * qreal(sample.height());
    const qreal fullArea   = qreal(image.width()) * qreal(image.height());

    if (sampleArea <= 0.0)
        return sampleBytes;

    qint64 estimate = qint64(sampleBytes * fullArea / sampleArea);

    // ── 修正量 ──
    // 小样本的固定开销（文件头、EXIF 占位等）被按比例放大了，需要扣回去。
    // 经验值：扣除约 1KB 的固定开销再放大，最后加回一个固定量。
    const qint64 fixedOverhead = 1024;
    if (sampleBytes > fixedOverhead) {
        estimate = qint64((sampleBytes - fixedOverhead) * fullArea / sampleArea)
                   + fixedOverhead;
    }

    return qMax<qint64>(0, estimate);
}

} // namespace zl
