#pragma once

// ============================================================================
//  Layer.h —— 单个图层
// ----------------------------------------------------------------------------
//  职责边界（重要）：
//    本类只是"一个带元信息的位图容器"，不含任何算法逻辑，也不知道 Document
//    的存在。所有图像运算都由 engine 层的纯函数完成，Layer 只负责存储结果。
//
//  位图格式铁律：
//    始终使用 QImage::Format_ARGB32_Premultiplied（预乘 alpha）。
//    预乘格式在合成时省去一次乘法，是合成性能的关键。但也带来一个陷阱：
//    所有基于 LUT 的调整必须先在非预乘空间运算（见 engine/Lut.cpp）。
// ============================================================================

#include "core/ImageTypes.h"

#include <QImage>
#include <QString>
#include <QRect>

namespace zl {

class Layer
{
public:
    Layer();
    explicit Layer(const QSize &size, const QString &name = QString());
    Layer(const QImage &image, const QString &name = QString());

    // ── 名称 ──
    QString name() const { return m_name; }
    void    setName(const QString &name) { m_name = name; }

    // ── 位图 ──
    // 返回 const 引用以避免不必要的深拷贝。注意：拿到引用后如果要修改，
    // 必须拷贝一份再改 —— QImage 是隐式共享的，直接改会污染所有共享者。
    const QImage &image() const { return m_image; }

    // 整体替换位图。会强制转换为预乘格式，保证格式铁律不被破坏。
    void setImage(const QImage &image);

    // 直接获取可写引用。调用前请确认没有其他对象正在共享这份数据。
    // 内部会做一次 convertToFormat（若已经是目标格式则是浅拷贝，几乎无成本）。
    QImage &mutableImage() { return m_image; }

    // ── 图层属性 ──
    quint8    opacity() const { return m_opacity; }          // 0~255，255 表示完全不透明
    void      setOpacity(quint8 value) { m_opacity = value; }
    qreal     opacityF() const { return m_opacity / 255.0; } // 便于参与浮点运算

    BlendMode blendMode() const { return m_blendMode; }
    void      setBlendMode(BlendMode mode) { m_blendMode = mode; }

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }

    // 锁定的图层不允许画笔、滤镜等修改像素的操作。
    // 注意：移动位置也不允许；但改名称、改不透明度仍允许。
    bool isLocked() const { return m_locked; }
    void setLocked(bool locked) { m_locked = locked; }

    // 是否为"文字图层"等特殊类型（Phase 6 会用到，先留字段）
    bool isSpecial() const { return m_special; }
    void setSpecial(bool special) { m_special = special; }

    // ── 几何 ──
    QSize size() const { return m_image.size(); }
    QRect rect() const { return QRect(QPoint(0, 0), m_image.size()); }
    bool  isNull() const { return m_image.isNull(); }

    // 便捷尺寸访问。
    //  ★ 存在的理由 ★
    //    Layer 的尺寸语义与 QImage 完全一致（它就是一张位图），
    //    而调用方大量需要"宽/高"这样的单值。若不加这两个函数，
    //    每个调用点都要写 layer.size().width() —— 冗长且容易写错。
    //    （曾经因为漏了它们而出现编译错误：Document.cpp 里用了
    //      adapted.width()，而 Layer 只有 size()。）
    int width()  const { return m_image.width(); }
    int height() const { return m_image.height(); }

    // 深拷贝（用于撤销快照）
    Layer clone() const;

    // 生成缩略图（图层面板用）。
    // size 为缩略图边长上限，返回图像会等比缩放并居中填充到 size × size 的
    // 透明底板上，这样图层面板里的缩略图尺寸统一，不会因长宽比不同而参差。
    QImage thumbnail(const QSize &size) const;

private:
    QImage    m_image;
    QString   m_name;
    quint8    m_opacity   = 255;
    BlendMode m_blendMode = BlendMode::Normal;
    bool      m_visible   = true;
    bool      m_locked    = false;
    bool      m_special   = false;
};

} // namespace zl
