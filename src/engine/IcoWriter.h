#pragma once

// ============================================================================
//  IcoWriter.h —— 把一张图编码成 ICO 图标文件（纯函数）
// ----------------------------------------------------------------------------
//  一句话：给定一张 QImage，产出 ICO 的**字节流**；本文件不建控件、不弹对话框、
//  不碰 Document —— 与 engine 其他文件同规矩，可以被离线自检直接断言。
//
//  ★★ 为什么自己写，而不是交给 Qt 的 qico 插件 ★★
//
//    ① **阈值没法表达**。产品口径是"超过 512×512 就不许存 ICO"，而
//       QImageWriter 只会闷头写下去（老版本 Qt 甚至静默缩到 256），
//       我们没有任何地方能插进这句判断。
//    ② **插件是要单独部署的**。qico.dll 属于 imageformats 插件，
//       漏打一个 .dll 的后果是"PNG 能存、ICO 存不了"，而且报错时机在运行期。
//       自己编码 = ICO 不依赖任何插件。
//    ③ **可以被断言**。文件结构、掩码、越界拒绝都能写成离线断言；
//       交给插件就只能"存一个看看能不能打开"。
//
//  ★★ ICO 里到底装了什么 ★★
//
//    固定头 ICONDIR（6 字节）+ 每个图标一条 ICONDIRENTRY（16 字节）
//    + 每个图标的图像数据。本实现**只写一条**（当前文档就一个尺寸），
//    所以偏移量是常量 22。
//
//    图像数据有两种存法，本实现按尺寸自动二选一：
//
//      · ≤ 256  → **BMP 条目**（老规矩、兼容性最好）：
//          40 字节 BITMAPINFOHEADER + 32bpp BGRA（**自下而上**存储）
//          + 1bpp 的 AND 掩码（同样自下而上、每行按 4 字节对齐）
//      · 257~512 → **PNG 条目**：整段 PNG 直接塞进去
//          为什么必须换：目录项里的宽/高**各只占 1 个字节**，
//          而其中 0 表示 256 —— 也就是说 BMP 条目在结构上就表达不了 256 以上。
//          PNG 条目是 Vista 之后 Windows 的官方扩展，尺寸只受 512 上限约束。
//          （此时目录项的宽高字节写 0；真正尺寸以 PNG 头为准，
//            Windows 与 Qt 都是这么读的 —— 见 ico_test 的往返断言。）
//
//  ★★ 上限 512 是"产品定案"，不是格式本身的硬边界 ★★
//
//    Windows 官方口径里图标最大 256×256；512 是各图标工具广泛接受的扩展上限。
//    超过 512 就不再是"图标"而是图片了，所以这里**直接拒绝**而不是默默缩小 ——
//    默默缩小会让用户以为"导出成功了"，回头发现图被改小，属于最难查的一类问题。
// ============================================================================

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

namespace zl {
namespace ico {

// 允许的最大边长（宽或高**任一**超过它就不许导出）。
constexpr int kMaxIconEdge = 512;

// BMP 条目能表达的最大边长。
//
// ★ 为什么是 256 而不是 255 ★
//   目录项里宽/高各占 1 字节，取值 0..255；**0 表示 256**。
//   所以 1~255 正常表达、256 用 0 表达、256 以上根本写不下。
constexpr int kBmpEntryMaxEdge = 256;

// ── 尺寸判定（界面层拿它决定"要不要拦下来提示用户"）──

// 尺寸是否落在可编码范围内（宽高都 > 0 且 ≤ 512）。
bool canEncode(const QSize &size);

// 被拒绝时给用户看的话。canEncode() 为真时返回空串。
//
// ★ 为什么让引擎层出这句话，而不是在界面层拼 ★
//   上限值 512 是引擎层的常量。界面层再写一遍 "512" 就有两份真相，
//   哪天上限改了，会出现"拦住用户的数字"和"真正生效的数字"不一致。
QString rejectionMessage(const QSize &size);

// 这个尺寸会用哪种条目存。
enum class EntryFormat {
    Bmp,   // ≤ 256：BITMAPINFOHEADER + BGRA + AND 掩码
    Png,   // 257~512：内嵌 PNG
};

EntryFormat entryFormatFor(const QSize &size);

// ── 编码 ──

// 把 image 编成完整的 ICO 字节流。失败时返回空 QByteArray
// （尺寸越界、图为空、PNG 编码失败），原因写进 errorOut。
QByteArray encode(const QImage &image, QString *errorOut = nullptr);

// 编码并写文件。失败原因写进 errorOut。
bool write(const QString &path, const QImage &image, QString *errorOut = nullptr);

} // namespace ico
} // namespace zl
