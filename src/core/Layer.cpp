#include "core/Layer.h"

#include <QPainter>

namespace zl {

Layer::Layer() = default;

Layer::Layer(const QSize &size, const QString &name)
    : m_name(name)
{
    // 新建图层默认全透明。注意必须显式 fill(Qt::transparent) ——
    // QImage 的构造函数不会清零内存，不填充的话内容是不可预测的垃圾数据。
    if (size.isValid() && !size.isEmpty()) {
        m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m_image.fill(Qt::transparent);
    }
}

Layer::Layer(const QImage &image, const QString &name)
    : m_name(name)
{
    setImage(image);
}

void Layer::setImage(const QImage &image)
{
    if (image.isNull()) {
        m_image = QImage();
        return;
    }

    // 强制统一为预乘格式。这是图层位图的铁律 ——
    // 合成算法（Blend.cpp）假定输入是预乘格式，若不统一会得到错误的半透明结果。
    // 若 image 已经是目标格式，convertToFormat 返回浅拷贝，几乎没有开销。
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        m_image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else {
        m_image = image;
    }
}

Layer Layer::clone() const
{
    Layer copy;
    copy.m_name      = m_name;
    copy.m_opacity   = m_opacity;
    copy.m_blendMode = m_blendMode;
    copy.m_visible   = m_visible;
    copy.m_locked    = m_locked;
    copy.m_special   = m_special;
    // 必须 copy()！QImage 是隐式共享的，直接赋值会让两个 Layer 共享同一份
    // 像素内存。撤销时修改"旧快照"会连带修改当前图像，撤销功能彻底失效。
    // 这个坑不报错，只是撤销时画面诡异，非常难查。
    copy.m_image = m_image.copy();
    return copy;
}

QImage Layer::thumbnail(const QSize &size) const
{
    if (m_image.isNull() || !size.isValid() || size.isEmpty())
        return QImage();

    // 先等比缩放到能放进 size 矩形的最大尺寸
    QImage scaled = m_image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 再贴到统一尺寸的透明底板上并居中。这样图层面板里所有缩略图大小一致，
    // 项目符号对齐，视觉上整齐；否则竖图和横图混排会非常难看。
    QImage canvas(size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int x = (size.width()  - scaled.width())  / 2;
    const int y = (size.height() - scaled.height()) / 2;
    painter.drawImage(x, y, scaled);
    painter.end();

    return canvas;
}

} // namespace zl
