#pragma once

// ============================================================================
//  ClipboardData.h —— 剪贴板数据结构
// ----------------------------------------------------------------------------
//  设计目标：同时满足"本程序内部精确粘贴"和"与外部程序互通"两个需求。
//
//  做法是向剪贴板写入两种 MIME 格式：
//    1. application/x-zlphotoedit-layer  —— 自定义格式，存像素 + 选区蒙版 +
//       原始位置。本程序粘贴时优先读它，能实现"复制了带羽化边缘的选区，
//       粘贴后边缘依然柔和"这种精确还原。
//    2. image/png  —— 标准格式，供其他程序（浏览器、聊天工具）粘贴使用。
//       同时它也让"从外部复制图片粘进来"成为可能。
//
//  不提供 image/bmp：BMP 是未压缩的，一张 4000×3000 的图进剪贴板有 48MB，
//  会导致系统剪贴板变慢甚至其他程序卡顿。PNG 压缩后小得多。
// ============================================================================

#include <QImage>
#include <QRect>
#include <QString>

namespace zl {

// MIME 类型常量。字符串里带程序名，避免与其他软件的自定义格式冲突。
extern const char *const kClipboardMimeLayer;   // "application/x-zlphotoedit-layer"

struct ClipboardData
{
    // ── 核心数据 ──

    // 像素数据。注意这是"已经按选区蒙版裁剪过 alpha"的结果 ——
    // 也就是说，若复制的是椭圆选区，这张图里椭圆外的像素 alpha 为 0。
    // 这个处理在复制的那一刻完成，粘贴时无需再关心选区形状。
    QImage pixels;

    // 选区蒙版（Format_Grayscale8），可为空。
    // 保留它是为了让"粘贴为新图层后再执行'载入选区'"这类操作可行，
    // 也为将来的"原位粘贴并恢复选区"预留。
    QImage mask;

    // 内容在**源文档**中的位置。用于"同文档内原位粘贴"。
    // 若为空矩形，表示来源不明（如来自其他程序），粘贴时按居中处理。
    QRect sourceBounds;

    // 来源文档的标识。用于判断"是不是同一个文档"。
    // 跨文档粘贴时不能原位，因为坐标系可能完全不同。
    quintptr sourceDocumentId = 0;

    // ── 状态 ──
    bool isValid() const
    {
        return !pixels.isNull() && pixels.width() > 0 && pixels.height() > 0;
    }

    bool hasMask() const { return !mask.isNull(); }

    // 是否来自指定文档（原位粘贴的前提条件）
    bool isFromDocument(quintptr docId) const
    {
        return docId != 0 && sourceDocumentId == docId && !sourceBounds.isNull();
    }

    // 内容尺寸
    QSize size() const { return pixels.size(); }

    // ── 序列化 ──
    // 打包为可放入 QMimeData 的字节流。
    // 格式：自定义头 + PNG 压缩的像素 + PNG 压缩的蒙版（若有）。
    // 用 PNG 而不是裸像素，是因为剪贴板数据要在系统里驻留，体积越小越好。
    QByteArray serialize() const;

    // 从字节流还原。失败时返回的对象 isValid() 为 false。
    static ClipboardData deserialize(const QByteArray &data);

    // 清空
    void clear()
    {
        pixels = QImage();
        mask = QImage();
        sourceBounds = QRect();
        sourceDocumentId = 0;
    }
};

} // namespace zl
