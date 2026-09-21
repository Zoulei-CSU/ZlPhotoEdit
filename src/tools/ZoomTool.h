#pragma once

// ============================================================================
//  ZoomTool.h —— 缩放（点击缩放 + 框选缩放）
// ----------------------------------------------------------------------------
//  两种操作合在一个工具里，靠"有没有真的拖动"区分：
//    · 点一下        → 以按下点为锚点放大/缩小一档
//    · 拖出一个框    → 把框住的区域铺满视口（业内叫"框选缩放"）
//  这也是 Photoshop 缩放工具的行为，用户不需要学第二套。
//
//  ★★ 阈值：手抖不能算框选 ★★
//
//    按下鼠标时手几乎一定会抖一两个像素。如果不设阈值，用户想"点一下放大"，
//    结果每次都变成"框了一个 2×3 像素的区域去放大" —— 画面会突然跳到一个
//    毫无意义的局部，而且是**随机**的（取决于抖了多少）。
//    所以拖动距离必须超过 marqueeThreshold()（6 个控件像素）才切换成框选。
//
//    6 这个数取的是"明确有意"与"手抖"之间的空档：正常点击的抖动实测 < 3px，
//    而有意拖框一般 > 20px。它被开成静态函数就是为了让自检用**同一个数字**
//    构造用例 —— 测试里写死 6、实现改成 8 的话，测试会以"手抖"的名义变红，
//    却把人引向一个错误的结论。
//
//  ★ 阈值判定必须用**控件坐标**，不能用图像坐标 ★
//
//    图像距离会随缩放变化：在 800% 下 6 个控件像素只对应 0.75 个图像像素，
//    用图像坐标判的话，放大状态下**永远触发不了框选**。
//    而用户感知到的"我移动了多少"，本来就是屏幕上的距离。
//
//  ★ 退化的框选要回落到单击，不能什么都不做 ★
//
//    框选区域换算到图像坐标后宽或高不足 1 个像素时（低缩放下常见），
//    zoomToRect 会算出一个荒谬的倍率（0.6 像素宽 → 几千倍）。
//    此时**不能什么都不做**：用户明明做了一个动作，没有任何反馈会像卡死。
//    回落成"在按下点放大一档"是最符合直觉的兜底。
//
//  ★ Alt 反转方向 ★
//
//    选项条上白纸黑字写着"按住 Alt 可临时切换到相反方向"，这里就是它的实现。
//    **框选缩放不受 Alt 影响** —— "把这一块放大"没有反方向可言
//    （缩小到某一个区域是没有意义的操作）。
//
//  ★ 步长 kZoomStepFactor 是共享常量 ★
//
//    "点一下缩放工具"与"按 Ctrl+= 放大"必须走同一档步长，
//    否则用户会认为其中一个是坏的。那个数字定义在 tools/ToolParams.h，
//    两边都从那里取（ui → tools 是允许的依赖方向）。
//
//  ★ 不按 isReady() 拒绝 ★
//
//    与抓手同理：缩放不碰像素。没有文档时缩放只是在缩放一片空白，
//    没有任何副作用，而"文档关掉后缩放工具突然失灵"会让用户以为程序坏了。
// ============================================================================

#include "tools/AbstractTool.h"

#include <QPointF>
#include <QRectF>

class QPainter;

namespace zl {

class ZoomOptions;

class ZoomTool : public AbstractTool
{
    Q_OBJECT

public:
    ZoomTool(Document *doc, CommandStack *stack, QObject *parent = nullptr);
    ~ZoomTool() override;

    // ── 身份 ──
    QString id() const override;
    QString displayName() const override;
    int     shortcutKey() const override;

    ToolOptions *options() const override;

    // ── 输入事件 ──
    ToolResult mousePress(const ToolEvent &e) override;
    ToolResult mouseMove(const ToolEvent &e) override;
    ToolResult mouseRelease(const ToolEvent &e) override;

    // 框选过程中的虚线框。
    // 走 paintScreenOverlay（控件坐标），所以缩放比变化时框线不会跟着变粗
    // —— 与裁剪框、吸管取样框同一套约定。
    void paintScreenOverlay(QPainter &painter, CanvasView *view) override;

    void cancel() override;

    // ── 给自检用的观测口 ──

    /// 是否已经进入框选模式（拖动距离超过了阈值）
    bool isMarqueeActive() const { return m_marqueeActive; }

    /// 按下点 → 当前点的归一化矩形（图像坐标）。未按下时是空矩形。
    QRectF marqueeImageRect() const;
    /// 同上，控件坐标（即屏幕上那个框）
    QRectF marqueeWidgetRect() const;

    /// 最近一次"点击缩放"的倍率与锚点（控件坐标）
    qreal   lastZoomFactor() const { return m_lastZoomFactor; }
    QPointF lastZoomAnchor() const { return m_lastZoomAnchor; }

    /// 最近一次"框选缩放"的矩形（图像坐标）
    QRectF  lastZoomToRect() const { return m_lastZoomToRect; }

    /// 点击缩放 / 框选缩放各发生过多少次（自检用它区分两条路径）
    int clickZoomCount() const { return m_clickZoomCount; }
    int marqueeZoomCount() const { return m_marqueeZoomCount; }

    /// 在给定修饰键下，本工具是"放大"还是"缩小"。
    /// 抽成公开函数是为了让自检直接验证"Alt 反转"这条约定，
    /// 而不必从一次点击的结果去反推。
    bool zoomsIn(bool altDown) const;

    /// 拖动超过这个距离（控件像素）才切换成框选模式。
    static qreal marqueeThreshold() { return 6.0; }

private:
    ZoomOptions *m_options = nullptr;

    bool    m_pressed = false;

    // 按下点（同时记控件与图像两套坐标）：
    //   控件坐标 → 点击缩放的锚点、框选阈值判定、叠加层画框
    //   图像坐标 → 框选缩放要放大的那块内容
    QPointF m_pressWidgetPos;
    QPointF m_pressImagePos;

    QPointF m_currentWidgetPos;
    QPointF m_currentImagePos;

    bool m_marqueeActive = false;

    // 观测用
    qreal   m_lastZoomFactor = 0.0;
    QPointF m_lastZoomAnchor;
    QRectF  m_lastZoomToRect;
    int     m_clickZoomCount   = 0;
    int     m_marqueeZoomCount = 0;
};

} // namespace zl
