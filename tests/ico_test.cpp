// ============================================================================
//  ico_test.cpp —— ICO 编码的离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    另存为 ICO 是一条**"错了也能写出一堆字节"**的路。写出来的文件
//    不会崩、不会报错，甚至本程序自己也不会去读它 —— 只有拿到别的程序里
//    （资源管理器、图标编辑器）才发现"打不开"或者"图是花的"。
//    这类问题靠人眼验收的代价极高，所以每一条都要能被机械断言。
//
//    本组从**三个角度**钉住它：
//
//      ① **字节级结构** —— ICONDIR / ICONDIRENTRY / BITMAPINFOHEADER 的
//         每个字段（含小端序、含"宽度 0 表示 256"这类陷阱）都对拍；
//         外加一条最强的整体不变量：`图像偏移 + 图像长度 == 文件长度`
//         （等于"文件里既没有缺口、也没有多余字节"，一处算错就红）。
//      ② **往返** —— 写出来之后用 **Qt 自带的 ico 读取器**读回来，
//         逐像素比。这条最像"用户真的能打开吗"，且 Qt 的读取器与我们的
//         编码器是两套独立实现，互相印证。
//      ③ **拒绝** —— 超过 512 的图必须**既返回失败、又不落文件**。
//
//  ★ 它刻意只链接 Qt5Core + Qt5Gui（不链 QtWidgets）★
//    编码是纯数据变换，不该有任何界面依赖。哪天有人在 IcoWriter 里
//    include 了 QWidget，这里会立刻编译失败。
// ============================================================================

#include "engine/IcoWriter.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdio>

using namespace zl;

static int g_failed = 0;
static int g_passed = 0;
static int g_group  = 0;

static void beginGroup(const char *name)
{
    ++g_group;
    std::printf("[%d] %s\n", g_group, name);
    std::fflush(stdout);
}

static void check(bool ok, const char *what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

static void checkInt(long long actual, long long expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %lld，实际 %lld\n",
                    what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkStr(const QString &actual, const QString &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 \"%s\"，实际 \"%s\"\n",
                    what, qUtf8Printable(expected), qUtf8Printable(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  字节读取（小端）
// ---------------------------------------------------------------------------
static quint16 rdU16(const QByteArray &a, int at)
{
    return quint16(quint8(a.at(at)))
         | quint16(quint8(a.at(at + 1))) << 8;
}

static quint32 rdU32(const QByteArray &a, int at)
{
    return quint32(quint8(a.at(at)))
         | quint32(quint8(a.at(at + 1))) << 8
         | quint32(quint8(a.at(at + 2))) << 16
         | quint32(quint8(a.at(at + 3))) << 24;
}

// BMP 条目的掩码行长（1bpp、每行补齐到 4 字节）
static int maskBytesPerLine(int w)
{
    return ((w + 31) / 32) * 4;
}

// ---------------------------------------------------------------------------
//  通用结构校验：对任何尺寸、任何条目格式都应成立
// ---------------------------------------------------------------------------
//  ★ 这条断言组是"整体不变量"型的 ★
//    单看某个字段写错一位，很难说清后果；但
//        dwImageOffset + dwBytesInRes == 文件总长
//    一旦不成立，就说明文件里要么缺一块、要么多一块 —— 任何读图程序
//    都会在读到一半时错位。它同时也就钉住了"条目里记的长度"与
//    "实际写出去的字节数"一致。
static void validateStructure(const QByteArray &ico, int w, int h, const char *what)
{
    std::printf("  · %s\n", what);

    check(ico.size() >= 22, "文件至少要有固定头 + 一条目录项");

    // ── ICONDIR ──
    checkInt(rdU16(ico, 0), 0, "idReserved == 0（规范要求）");
    checkInt(rdU16(ico, 2), 1, "idType == 1（图标；2 是光标）");
    checkInt(rdU16(ico, 4), 1, "idCount == 1（本实现只写一条）");

    // ── ICONDIRENTRY ──
    //
    //  ★★ 宽/高字段只有 1 字节，0 表示 256 ★★
    //    所以它**最多只能表达 256**：BMP 条目（≤256）能如实表达；
    //    而**任何一边超过 256** 时那个字节只能写 0，真实尺寸落在 PNG 头里，
    //    由 ④ 组的往返断言（读回来的宽高必须等于 w/h）来钉。
    //
    //  ★ 注意别写成"PNG 条目两个字节都必须是 0" ★
    //    300×100 这种"只有一边超"的图是合法的 PNG 条目，
    //    它的高字节该写 100（如实表达），不是 0。
    //    统一按 `edge >= 256 ? 0 : edge` 判，两种条目都覆盖得到。
    const ico::EntryFormat fmt = ico::entryFormatFor(QSize(w, h));
    checkInt(quint8(ico.at(6)), (w >= 256 ? 0 : w),
             "目录项宽度字节（0 表示 256；>256 只能写 0）");
    checkInt(quint8(ico.at(7)), (h >= 256 ? 0 : h),
             "目录项高度字节（同理）");
    checkInt(quint8(ico.at(8)), 0, "bColorCount == 0（≥8bpp 的约定）");
    checkInt(quint8(ico.at(9)), 0, "bReserved == 0");
    checkInt(rdU16(ico, 10), 1, "wPlanes == 1");
    checkInt(rdU16(ico, 12), 32, "wBitCount == 32");

    const quint32 bytesInRes = rdU32(ico, 14);
    const quint32 imageOffset = rdU32(ico, 18);

    checkInt(imageOffset, 22, "图像从第 22 字节开始（6 + 16）");
    // ★★ 整体不变量 ★★
    checkInt(qint64(imageOffset) + qint64(bytesInRes), qint64(ico.size()),
             "★★ 图像偏移 + 图像长度 == 文件总长（既无缺口也无冗余字节）");

    // ── 图像数据 ──
    if (fmt == ico::EntryFormat::Bmp) {
        const int at = 22;
        checkInt(rdU32(ico, at + 0), 40, "BITMAPINFOHEADER.biSize == 40");
        checkInt(rdU32(ico, at + 4), w, "biWidth");
        // ★ 高度记的是 XOR 图 + AND 掩码的**合计**高度 ★
        checkInt(rdU32(ico, at + 8), h * 2, "★★ biHeight == 2 × 高（XOR + AND 掩码合计）");
        checkInt(rdU16(ico, at + 12), 1, "biPlanes == 1");
        checkInt(rdU16(ico, at + 14), 32, "biBitCount == 32");
        checkInt(rdU32(ico, at + 16), 0, "biCompression == BI_RGB(0)");

        const int colorBytes = w * 4 * h;
        const int maskBytes  = maskBytesPerLine(w) * h;
        checkInt(rdU32(ico, at + 20), colorBytes + maskBytes, "biSizeImage == 像素 + 掩码");
        checkInt(qint64(bytesInRes), 40 + colorBytes + maskBytes,
                 "dwBytesInRes == 头 + 像素 + 掩码");
    } else {
        // PNG 条目的"真尺寸"只能去 PNG 头里读 —— 上面已经把
        // "目录项字节写 0"这件事断言过了，这里只确认那段真的是 PNG。
        static const uchar kPngMagic[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        bool magicOk = bytesInRes >= 8;
        for (int i = 0; magicOk && i < 8; ++i)
            magicOk = (quint8(ico.at(int(imageOffset) + i)) == kPngMagic[i]);
        check(magicOk, "★★ PNG 条目里真的是一段 PNG（8 字节魔数对得上）");
    }
}

// ---------------------------------------------------------------------------
//  造测试图
// ---------------------------------------------------------------------------
//  刻意用**非预乘 ARGB32** 作为输入是远远不够的 —— 本工程内部一律预乘，
//  所以另有一组专门喂预乘数据（见 ⑥）。
static QImage makeImage(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            // 四象限分别是不透明红 / 半透明绿 / 全透明 / 不透明蓝
            const bool right = x >= w / 2;
            const bool bottom = y >= h / 2;
            if (!right && !bottom)
                row[x] = qRgba(255, 0, 0, 255);
            else if (right && !bottom)
                row[x] = qRgba(0, 255, 0, 128);
            else if (!right && bottom)
                row[x] = qRgba(0, 0, 0, 0);
            else
                row[x] = qRgba(0, 0, 255, 255);
        }
    }
    return img;
}

// ===========================================================================
//  ① 尺寸判定与上限
// ===========================================================================
static void testSizeRules()
{
    beginGroup("尺寸判定：上限 512，边界必须精确");

    check(ico::canEncode(QSize(1, 1)), "1×1 可以编码");
    check(ico::canEncode(QSize(16, 16)), "16×16（标准图标尺寸）");
    check(ico::canEncode(QSize(256, 256)), "256×256（BMP 条目的上限）");
    check(ico::canEncode(QSize(512, 512)), "★ 512×512 放行 —— 恰好等于上限，而不是【超过】");

    check(!ico::canEncode(QSize(513, 512)), "★ 513×512 拒绝（宽超 1 像素也要拦）");
    check(!ico::canEncode(QSize(512, 513)), "★ 512×513 拒绝（高同理）");
    check(!ico::canEncode(QSize(4000, 3000)), "4000×3000（照片）拒绝");
    check(!ico::canEncode(QSize(0, 100)), "宽为 0 拒绝");
    check(!ico::canEncode(QSize(100, 0)), "高为 0 拒绝");
    check(!ico::canEncode(QSize(-1, 10)), "负尺寸拒绝");
    check(!ico::canEncode(QSize()), "无效尺寸拒绝");

    // ── 提示语 ──
    check(ico::rejectionMessage(QSize(512, 512)).isEmpty(),
          "能编码时提示语为空（调用方据此判断要不要弹窗）");

    const QString msg = ico::rejectionMessage(QSize(600, 400));
    check(!msg.isEmpty(), "超限时有提示语");
    check(msg.contains(QStringLiteral("600")), "提示语里带实际宽度");
    check(msg.contains(QStringLiteral("400")), "提示语里带实际高度");
    check(msg.contains(QStringLiteral("512")), "提示语里带上限值");
    // ★ 上限值只应该来自引擎层的常量 ★
    checkStr(QString::number(ico::kMaxIconEdge), QStringLiteral("512"),
             "上限常量就是 512");

    check(ico::rejectionMessage(QSize(0, 0)).contains(QStringLiteral("无效")),
          "尺寸无效时给的是另一句话（不是【太大】）");
}

// ===========================================================================
//  ② 条目格式的选择
// ===========================================================================
static void testEntryFormat()
{
    beginGroup("条目格式：≤256 走 BMP，257~512 走 PNG");

    check(ico::entryFormatFor(QSize(16, 16)) == ico::EntryFormat::Bmp, "16 → BMP");
    check(ico::entryFormatFor(QSize(255, 255)) == ico::EntryFormat::Bmp, "255 → BMP");
    check(ico::entryFormatFor(QSize(256, 256)) == ico::EntryFormat::Bmp,
          "★ 256 → BMP（0 字节刚好能表示 256，还在 BMP 的能力范围内）");
    check(ico::entryFormatFor(QSize(257, 257)) == ico::EntryFormat::Png,
          "★ 257 → PNG（BMP 条目的宽高各只有 1 字节，装不下）");
    check(ico::entryFormatFor(QSize(512, 512)) == ico::EntryFormat::Png, "512 → PNG");
    check(ico::entryFormatFor(QSize(600, 20)) == ico::EntryFormat::Png,
          "★ 只要**任一维**超 256 就得走 PNG（600×20 也不例外）");
    check(ico::entryFormatFor(QSize(20, 600)) == ico::EntryFormat::Png, "高同理");
}

// ===========================================================================
//  ③ 字节级结构
// ===========================================================================
static void testStructure()
{
    beginGroup("字节级结构：目录项 / BMP 头 / 掩码");

    // ── 4×4 的小图，逐字节对拍 ──
    QImage img(4, 4, QImage::Format_ARGB32);
    img.fill(QColor(255, 0, 0, 255));
    img.setPixelColor(0, 0, QColor(0, 0, 0, 0));          // 全透明
    img.setPixelColor(1, 1, QColor(0, 255, 0, 128));      // 半透明绿

    QString err;
    const QByteArray ico = ico::encode(img, &err);
    check(!ico.isEmpty(), "4×4 编码成功");
    check(err.isEmpty(), "成功时不设错误信息");

    validateStructure(ico, 4, 4, "4×4（BMP 条目）");

    // 手工算：6 + 16 + 40 + 4*4*4 + (((4+31)/32)*4)*4 = 142
    checkInt(ico.size(), 6 + 16 + 40 + 64 + 16, "总长度 = 头 22 + BMP 头 40 + 像素 64 + 掩码 16");

    // ── 像素：自下而上、B G R A ──
    //   第一个像素是**左下角** (0, 3)，它是左上角填充色 → 不透明红
    const int px = 22 + 40;
    checkInt(quint8(ico.at(px + 0)), 0,   "首个像素 B == 0");
    checkInt(quint8(ico.at(px + 1)), 0,   "首个像素 G == 0");
    checkInt(quint8(ico.at(px + 2)), 255, "首个像素 R == 255");
    checkInt(quint8(ico.at(px + 3)), 255, "首个像素 A == 255");
    checkInt(quint8(ico.at(px - 16 + 0)), 0,
             "★ 第一行第一个像素 = 图像**左下角**（行序自下而上）");

    // 半透明绿在 (1,1)。它在文件里的位置：
    //   自下而上第 (3 - 1) = 2 行，行内第 1 个像素 → 偏移 px + 2*16 + 1*4
    const int greenAt = px + 2 * (4 * 4) + 1 * 4;
    checkInt(quint8(ico.at(greenAt + 0)), 0,   "半透明像素 B == 0");
    checkInt(quint8(ico.at(greenAt + 1)), 255, "半透明像素 G == 255");
    checkInt(quint8(ico.at(greenAt + 2)), 0,   "半透明像素 R == 0");
    checkInt(quint8(ico.at(greenAt + 3)), 128, "★ 半透明像素 A == 128（没有被抹成 0 / 255）");

    // ── AND 掩码 ──
    //   ★ 只有**全透明**的像素才置位，半透明不置位 ★
    //   掩码也自下而上：(0,0) 那一行是**最后**写的。
    //   4 像素宽 → 行长 1 字节，补齐到 4 字节 → 每行 4 字节。
    const int maskAt = px + 4 * 4 * 4;
    const int mbl = maskBytesPerLine(4);
    checkInt(mbl, 4, "4 像素宽的掩码行长补齐到 4 字节");

    checkInt(quint8(ico.at(maskAt + 0 * mbl)), 0x00, "掩码 y=3 行全 0");
    checkInt(quint8(ico.at(maskAt + 1 * mbl)), 0x00, "掩码 y=2 行全 0");
    checkInt(quint8(ico.at(maskAt + 2 * mbl)), 0x00,
             "★ 掩码 y=1 行全 0 —— 半透明像素**不**置位（置位会把它挖成透明）");
    checkInt(quint8(ico.at(maskAt + 3 * mbl)), 0x80,
             "★★ 掩码 y=0 行 = 0x80：最左像素（全透明）置位，位序是**最高位在左**");

    // ── 多档尺寸的通用结构校验 ──
    struct Case { int w; int h; };
    const Case cases[] = {
        { 1, 1 }, { 16, 16 }, { 32, 48 }, { 255, 255 }, { 256, 256 },
        { 257, 257 }, { 300, 300 }, { 512, 300 }, { 512, 512 },
        // ★ 只有一边超 256 —— 目录项里"另一边的字节"是能如实表达的，
        //   别把它也当成 0（这条用例一开始漏了，断言也因此写错过一次）。
        { 300, 100 }, { 100, 300 }, { 512, 20 },
    };
    for (const Case &c : cases) {
        const QImage img2 = makeImage(c.w, c.h);
        QString e2;
        const QByteArray bytes = ico::encode(img2, &e2);
        if (bytes.isEmpty()) {
            ++g_failed;
            std::printf("  [失败] %d×%d 编码返回空：%s\n",
                        c.w, c.h, qUtf8Printable(e2));
            continue;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%d×%d", c.w, c.h);
        validateStructure(bytes, c.w, c.h, label);
    }

    // 目录项里"0 表示 256"这一条要单独钉一次
    const QByteArray sq = ico::encode(makeImage(256, 256), nullptr);
    checkInt(quint8(sq.at(6)), 0, "★ 256×256 的目录项宽度字节写 0（0 即 256）");
    checkInt(quint8(sq.at(7)), 0, "★ 256×256 的目录项高度字节写 0");
}

// ===========================================================================
//  ④ 往返：用 Qt 自带的 ico 读取器读回来
// ===========================================================================
//  ★ 为什么这条最有价值 ★
//    Qt 的 ico 读取器与我们这个编码器是**两套独立实现**。
//    它能把我们的文件读回来、且像素对得上，说明我们产出的不是
//    "自己看着顺眼"的字节，而是真的符合格式。
static void testRoundTrip()
{
    beginGroup("往返：写出去 → 用 Qt 的读取器读回来 → 逐像素比");

    QTemporaryDir dir;
    check(dir.isValid(), "能创建临时目录");
    if (!dir.isValid())
        return;

    struct Case { int w; int h; const char *note; };
    const Case cases[] = {
        { 16,  16,  "16×16（BMP 条目）" },
        { 32,  32,  "32×32（BMP 条目）" },
        { 48,  48,  "48×48（BMP 条目）" },
        { 256, 256, "256×256（BMP 条目上限）" },
        { 300, 300, "★★ 300×300（PNG 条目）" },
        { 512, 512, "★★ 512×512（PNG 条目上限）" },
        // ★ 一边超、另一边不超：目录项里"不超的那一边"是如实写的，
        //   得确认 Qt 的读取器在这种情况下照样读对（它走的是 PNG 头）。
        { 400, 120, "★★ 400×120（只有一边超 256）" },
        { 120, 400, "★★ 120×400（另一边超）" },
    };

    for (const Case &c : cases) {
        const QImage src  = makeImage(c.w, c.h);
        const QString path = dir.filePath(QStringLiteral("icon_%1.ico").arg(c.w));

        QString err;
        if (!ico::write(path, src, &err)) {
            ++g_failed;
            std::printf("  [失败] %s 写文件失败：%s\n", c.note, qUtf8Printable(err));
            continue;
        }

        // ★ 用 QImageReader 显式指定 ico，避免"文件损坏时静默回落到别的格式" ★
        QImageReader reader(path, "ico");
        const QImage back = reader.read();
        if (back.isNull()) {
            ++g_failed;
            std::printf("  [失败] %s 读不回来：%s\n",
                        c.note, qUtf8Printable(reader.errorString()));
            continue;
        }

        checkInt(back.width(),  c.w, "读回来的宽一致");
        checkInt(back.height(), c.h, "读回来的高一致");

        // 抽查四象限 + 中心，覆盖 不透明 / 半透明 / 全透明 三种 alpha
        const int xs[] = { 0, c.w / 4, c.w / 2 + c.w / 4, c.w - 1 };
        const int ys[] = { 0, c.h / 4, c.h / 2 + c.h / 4, c.h - 1 };
        int mismatched = 0;      // 颜色不符（对非透明像素）
        int alphaLost  = 0;      // 该不透明的被读成透明、或反之
        for (int yi = 0; yi < 4; ++yi) {
            for (int xi = 0; xi < 4; ++xi) {
                const QColor want = src.pixelColor(xs[xi], ys[yi]);
                const QColor got  = back.pixelColor(xs[xi], ys[yi]);
                if (want.alpha() != got.alpha())
                    ++alphaLost;
                // 全透明像素的 RGB 没有意义（很多编码器会清零），只比 alpha
                if (want.alpha() != 0) {
                    if (qAbs(want.red()   - got.red())   > 1
                     || qAbs(want.green() - got.green()) > 1
                     || qAbs(want.blue()  - got.blue())  > 1)
                        ++mismatched;
                }
            }
        }
        char label[128];
        std::snprintf(label, sizeof(label), "%s：颜色一致", c.note);
        checkInt(mismatched, 0, label);
        std::snprintf(label, sizeof(label), "%s：alpha 一致（含半透明）", c.note);
        checkInt(alphaLost, 0, label);

        // 顺带把"半透明真的活着"单独钉一次 —— 这是 BMP 条目最容易丢的东西
        if (c.w >= 4 && c.h >= 4) {
            const QColor wantSemi = src.pixelColor(c.w / 2 + c.w / 4, c.h / 4);
            const QColor gotSemi  = back.pixelColor(c.w / 2 + c.w / 4, c.h / 4);
            if (wantSemi.alpha() > 0 && wantSemi.alpha() < 255) {
                char semiLabel[160];
                std::snprintf(semiLabel, sizeof(semiLabel),
                              "★ %s：半透明像素读回来仍是半透明（A=%d → %d）",
                              c.note, wantSemi.alpha(), gotSemi.alpha());
                check(gotSemi.alpha() == wantSemi.alpha(), semiLabel);
            }
        }
    }
}

// ===========================================================================
//  ⑤ 拒绝：超过 512 必须失败，且**不落文件**
// ===========================================================================
static void testRejection()
{
    beginGroup("拒绝：>512 不编码、不落文件");

    QTemporaryDir dir;
    if (!dir.isValid()) {
        ++g_failed;
        std::printf("  [失败] 无法创建临时目录\n");
        return;
    }

    const QImage big(600, 400, QImage::Format_ARGB32);
    QString err;
    const QByteArray bytes = ico::encode(big, &err);
    check(bytes.isEmpty(), "★ 600×400 编码直接返回空");
    check(!err.isEmpty(), "★ 并且给出了错误信息");
    check(err.contains(QStringLiteral("600")) && err.contains(QStringLiteral("512")),
          "★ 错误信息里同时有实际尺寸与上限");

    const QString path = dir.filePath(QStringLiteral("too_big.ico"));
    QString err2;
    check(!ico::write(path, big, &err2), "★ 600×400 写文件返回失败");
    check(!QFile::exists(path),
          "★★ 失败时**不落下任何文件**（半成品比没有更糟：用户会以为存成功了）");
    check(!err2.isEmpty(), "写失败时也给出原因");

    // 另一个方向：只有一边超
    QString err3;
    check(ico::encode(QImage(20, 600, QImage::Format_ARGB32), &err3).isEmpty(),
          "★ 20×600（只有高超）同样拒绝");

    // 空图
    QString err4;
    check(ico::encode(QImage(), &err4).isEmpty(), "空图拒绝");
    check(!err4.isEmpty(), "空图给出原因");
}

// ===========================================================================
//  ⑥ 预乘数据不能串味
// ===========================================================================
//  ★★ 这是本项目最容易踩、也最难发现的一类坑 ★★
//    工程内部一律 ARGB32_Premultiplied。若编码时直接把内存里的 RGB
//    当"真颜色"写出去，半透明区域会明显偏暗 ——
//    而不透明像素完全正常，所以只有带透明边缘的图才会暴露。
//    这里喂一张预乘图，然后逐字节验文件里存的是**非预乘**值。
static void testPremultipliedInput()
{
    beginGroup("预乘输入：写进文件前必须先还原成非预乘");

    QImage pre(2, 1, QImage::Format_ARGB32_Premultiplied);
    // 半透明红：真颜色 (255,0,0) + alpha 128
    //   预乘后内存里是 (128, 0, 0, 128)
    pre.setPixelColor(0, 0, QColor(255, 0, 0, 128));
    pre.setPixelColor(1, 0, QColor(255, 255, 255, 255));

    // ★ 前提必须用 pixel() 读，不能用 pixelColor() ★
    //   pixelColor() 会**按格式还原**（预乘 → 非预乘），红又变回 255，
    //   看起来"预乘根本没有发生"，这条前提就废了。
    //   pixel() 给的是内存里的原始 QRgb。
    check(qRed(pre.pixel(0, 0)) < 200,
          "断言前提：预乘数据的内存里那一点的红已经不是 255 了");

    QString err;
    const QByteArray ico = ico::encode(pre, &err);
    check(!ico.isEmpty(), "预乘图能编码");
    if (ico.isEmpty())
        return;

    // 只有一个像素高 → 文件里第一个像素就是 (0,0)
    const int px = 22 + 40;
    checkInt(quint8(ico.at(px + 0)), 0,   "★ 预乘输入的半透明像素：B == 0");
    checkInt(quint8(ico.at(px + 1)), 0,   "★ 预乘输入的半透明像素：G == 0");
    checkInt(quint8(ico.at(px + 2)), 255,
             "★★ 预乘输入的半透明像素：R == 255（**不是 128** —— 已还原成非预乘）");
    checkInt(quint8(ico.at(px + 3)), 128, "★ 预乘输入的半透明像素：A == 128");

    // 顺带确认往返也对得上
    QTemporaryDir dir;
    if (dir.isValid()) {
        const QString path = dir.filePath(QStringLiteral("pre.ico"));
        if (ico::write(path, pre, nullptr)) {
            const QImage back = QImageReader(path, "ico").read();
            check(!back.isNull(), "预乘图往返：能读回来");
            if (!back.isNull()) {
                const QColor c = back.pixelColor(0, 0);
                check(c.red() > 250 && c.alpha() == 128,
                      "★★ 预乘图往返后半透明像素仍是 (255,·,·,128)");
            }
        }
    }
}

// ===========================================================================
//  ⑦ 尺寸报错的文案不能再出现"看起来像 bug"的数字
// ===========================================================================
static void testMessageQuality()
{
    beginGroup("提示语本身也要正确（用户唯一能看到的证据）");

    const QString m = ico::rejectionMessage(QSize(1024, 768));
    check(m.contains(QStringLiteral("1024")) && m.contains(QStringLiteral("768")),
          "提示语给出用户图的**真实**尺寸");
    check(m.contains(QStringLiteral("512")), "提示语给出上限");
    check(m.contains(QStringLiteral("图像大小")) || m.contains(QStringLiteral("缩小")),
          "★ 提示语要告诉用户【怎么办】，而不只是【不行】");
    check(!m.contains(QStringLiteral("%1")) && !m.contains(QStringLiteral("%3")),
          "★★ 占位符全部被替换掉了（漏 .arg 会让用户看到【%3】）");
}

// ===========================================================================

int main()
{
    std::printf("=== ico_test：ICO 图标编码 ===\n\n");

    testSizeRules();
    testEntryFormat();
    testStructure();
    testRoundTrip();
    testRejection();
    testPremultipliedInput();
    testMessageQuality();

    std::printf("\n----------------------------------------\n");
    std::printf("ico_test: 通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
