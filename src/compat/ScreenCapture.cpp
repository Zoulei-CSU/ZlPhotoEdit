#include "compat/ScreenCapture.h"

#include <QCursor>
#include <QGuiApplication>
#include <QPixmap>
#include <QRect>
#include <QScreen>
#include <QSize>

namespace zl {

// ---------------------------------------------------------------------------
//  选屏：光标所在的那一块，取不到回落主屏
// ---------------------------------------------------------------------------
//  ★ 为什么不用 QGuiApplication::screens().first() ★
//    那等价于"永远截主屏"。副屏用户按快捷键时截到的是另一块屏幕的内容，
//    而且不会有任何提示 —— 属于"看起来正常工作、结果全错"的那类行为。
static QScreen *screenUnderCursor()
{
    if (!QGuiApplication::instance())
        return nullptr;

    if (QScreen *s = QGuiApplication::screenAt(QCursor::pos()))
        return s;

    // 光标不在任何屏幕上（多屏之间的空隙、或离屏平台没有真实光标）
    return QGuiApplication::primaryScreen();
}

QImage grabScreen()
{
    QScreen *screen = screenUnderCursor();
    if (!screen)
        return QImage();          // 没有屏幕：契约③ —— 空图，由调用方报

    // ── 抓 ──
    //   window == 0 表示"屏幕本身"。注意 Qt 的文档明确写了：
    //   抓的是**屏幕上的像素**，所以压在上面的其它窗口会被一起抓进去 ——
    //   这正是"截图"要的语义（我们要的就是用户眼里的画面）。
    //   鼠标指针一般不抓（Qt 各平台实现略有差异，这里不依赖它）。
    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull())
        return QImage();

    QImage img = shot.toImage();
    if (img.isNull())
        return QImage();

    const qreal dpr = img.devicePixelRatio() > 0.0 ? img.devicePixelRatio() : 1.0;

    // ── 对账：抓回来的是"这一块屏"还是"整块虚拟桌面" ──
    //
    //  ★ 这里刻意不猜 Qt 的行为 ★
    //    Qt 没有承诺 `grabWindow(0)` 在 `window == 0` 时抓的是
    //    "这个 QScreen 那一块" 还是 "所有屏幕拼成的虚拟桌面"，
    //    实测这段语义随平台与版本而变（Windows 上多屏时常常是整块桌面）。
    //    猜错的后果不是崩溃，而是**截出来的图比屏幕大一圈、边上多出别的
    //    显示器的内容** —— 用户只会觉得"这工具怎么老是截错"。
    //
    //    所以：抓完之后**量一次尺寸**，只有确实抓回了整块桌面时才裁。
    //    两条路径（抓单屏 / 抓整桌面）因此都落到同一个结果上。
    const QSize grabbedLogical(qRound(img.width()  / dpr),
                               qRound(img.height() / dpr));

    if (grabbedLogical != screen->geometry().size()
        && grabbedLogical == screen->virtualGeometry().size()) {

        // 整块桌面的原点 = virtualGeometry().topLeft()，
        // 目标屏那块是它内部的一个偏移矩形（逻辑坐标），再乘 dpr 换成设备像素。
        const QRect logical = screen->geometry().translated(
            -screen->virtualGeometry().topLeft());
        const QRect device(qRound(logical.x()      * dpr),
                           qRound(logical.y()      * dpr),
                           qRound(logical.width()  * dpr),
                           qRound(logical.height() * dpr));

        const QImage cropped = img.copy(device);
        if (!cropped.isNull())
            img = cropped;
        // 裁不出来（尺寸对不上到连 copy 都失败）也照样往下走：
        // ★ 宁可给一张"大了一圈但内容完整"的图，也不要给一张空的 ★
        //   大图用户能自己裁，空图只能重来。
    }

    // ── 契约②：dpr 归一 ──
    img.setDevicePixelRatio(1.0);
    return img;
}

} // namespace zl
