#pragma once

// ============================================================================
//  CanvasView.h —— 画布控件
// ----------------------------------------------------------------------------
//  这是整个程序交互的心脏。职责三块：**视图变换**、**输入派发**、**呈现**。
//
//  ★★ 核心决策：视图状态用 QTransform 建模，不用 zoom + offset 两个标量 ★★
//
//    标量方案（zoom + offsetX + offsetY）只能表达"缩放 + 平移"。
//    一旦需要"旋转查看画布"（阶段七的视图旋转、按图像方向自动摆正），
//    所有基于标量的坐标换算都要返工 —— 而且返工点散落在每个工具里。
//
//    QTransform 方案则天然支持缩放、平移、旋转、翻转的任意组合，
//    且"以某点为锚点缩放"用矩阵表达是一行代码（见 setZoomAt）。
//
//    代价：坐标换算要走一次矩阵求逆（约 20 次浮点运算），
//    对鼠标事件来说完全可忽略。
//
//  ★ 呈现管线的 10 步顺序 ★
//    第 2 步 setTransform 之后，所有绘制都用图像坐标 —— 这是矩阵化的核心收益。
//    但需要"屏幕尺寸恒定"的装饰（角柄、笔刷轮廓）必须在第 10 步
//    resetTransform 之后画，否则放大到 800% 时它们会被放大成一堆巨大的图形。
//
//  ★ 通用手势由 CanvasView 自己拦截，不传给工具 ★
//    · 空格 + 拖动  → 平移
//    · 中键拖动     → 平移
//    · Ctrl + 滚轮  → 缩放（以鼠标为锚点）
//    · 滚轮         → 缩放（可配置为滚动）
//    · 方向键/Home/End → 滚动（没有工具消费时）
//    这些是"视图操作"，与具体工具无关，让每个工具都实现一遍毫无意义。
// ============================================================================

#include <QWidget>
#include <QTransform>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QPoint>
#include <QPointF>
#include <QColor>
#include <QFont>
#include <QSize>
#include <QTimer>
#include <QElapsedTimer>
#include <QPainterPath>
#include <QVector>

// ★ 必须包含 ToolEvent.h 的完整定义（不能只做前置声明）★
//
//   本类的两个私有成员函数按**值**返回/接收 ToolEvent：
//       ToolEvent makeToolEvent(...) const;              // 返回值
//       void handleToolResult(const ToolResult &, const ToolEvent &);
//   返回值类型必须是完整类型 —— 编译器要为其分配栈空间。
//   只前置声明会报 "使用了未定义类型" 或 "函数不接受 N 个参数"。
//
//   而 ToolEventType 是 ToolEvent.h 里的枚举，前置声明根本无法引入它，
//   报错是 "语法错误: 标识符 ToolEventType"（看起来完全不像缺 include）。
//
//   ToolEvent.h 只依赖 QtCore/QtGui，不含 QtWidgets，所以把它引进
//   这个头文件不会造成分层污染。
#include "tools/ToolEvent.h"

// ★ 前置声明必须写在**全局作用域**，不能写进 namespace zl ★
//   写进去就变成 `zl::QTextEdit` / `zl::QFont` —— 与真的 Qt 类毫无关系，
//   编译器只会报"使用了未定义类型"，而且更阴的一点是：
//   凡是 `using namespace zl;` 之后的代码，写裸 `QFont` 也会解析到
//   这个假的 `zl::QFont` 上，于是**别的文件**跟着报一串莫名其妙的错
//   （实测 MainWindowActions.cpp 里一个无辜的 `QFont f;` 也红了）。
//   这类"一个声明污染整个命名空间"的错误，报错位置离病根很远。
//
//   QTextEdit 只作为**指针**成员出现，前置声明足够；
//   QFont 按值作成员，必须真的 include（见上面的 <QFont>）。
class QTextEdit;

namespace zl {

class Document;
class AbstractTool;

// ============================================================================
//  Guide —— 一根参考线
// ----------------------------------------------------------------------------
//  参考线是**视图辅助**，不是图像内容：
//    · 它**不会**进入 Document::composite()（合成管线根本不知道它的存在），
//      所以"导出时不可见"是结构性保证，不是靠某个 if 记得跳过；
//    · 它**每页一份**（存在 CanvasView 里）—— 与缩放/平移同属"视图状态"，
//      切标签页时跟着那一页走，而不是全局共享（见 docs/架构规划.md §3.6 第 7 条）。
//
//  ★ position 的语义跟 orientation 绑在一起，容易记反 ★
//    水平线（orientation == Qt::Horizontal）横着躺，它的 position 是 **y**；
//    竖直线（Qt::Vertical）立着站，它的 position 是 **x**。
//    本文件里所有相关代码都按这条写，不再另设 getter 去"猜"。
// ============================================================================
struct Guide {
    Qt::Orientation orientation = Qt::Horizontal;
    int             position    = 0;      // 图像坐标（水平线给 y / 竖直线给 x）
    bool            locked      = false;  // 单根锁（全局锁见 setGuidesLocked）
};

class CanvasView : public QWidget
{
    Q_OBJECT

public:
    explicit CanvasView(QWidget *parent = nullptr);
    ~CanvasView() override;

    // ── 文档 ──
    //
    // ★ 遵守约定 7：用 setDocument() 而不是构造时绑定 ★
    //   构造时绑定的话，多标签化时每个标签都要重建 CanvasView；
    //   setDocument 则可以复用同一个控件，切换文档时只是换个指针。
    void      setDocument(Document *doc);
    Document *document() const { return m_doc; }

    // ── 诊断：选区轮廓"真的重建了几次" ──
    //
    // ★ 为什么这张计数器必须存在 ★
    //   轮廓重建这件事**从画面上完全看不出来**：
    //     · 重建得太频繁 → 没有任何外部症状，只是慢（全选时一次几十毫秒，
    //       而它在"每个画笔事件"里都发生过）；
    //     · 该重建却没重建 → 症状是"蚂蚁线不跟着选区走"。
    //   没有这个计数，自检只能验"轮廓画得对不对"，
    //   **验不了"有没有白算"** —— 那正是本次改动要消灭的东西。
    //   （与 Document::fullCompositeCount() 同一套路，见铁律 §20.5。）
    quint64 outlineRebuildCount() const { return m_outlineRebuildCount; }

    // ── 工具 ──
    //
    // 借用指针，不接管所有权。传 nullptr 表示没有活动工具
    // （此时画布仍然可以平移缩放）。
    void          setActiveTool(AbstractTool *tool);
    AbstractTool *activeTool() const { return m_activeTool; }

    // ── 颜色 ──
    // 工具的颜色来源统一从这里取，避免每个工具各存一份。
    void   setForegroundColor(const QColor &c);
    void   setBackgroundColor(const QColor &c);
    QColor foregroundColor() const { return m_foregroundColor; }
    QColor backgroundColor() const { return m_backgroundColor; }

    // ── 视图变换 ──

    // 图像坐标 → 控件坐标的变换矩阵。
    // 这是**唯一的**视图状态源，其他访问器都是从这里派生出来的。
    QTransform viewTransform() const { return m_viewTransform; }
    void       setViewTransform(const QTransform &t);

    // 缩放因子（矩阵的近似均匀缩放分量）。
    // 注意：如果做过视图旋转，这个值取自矩阵的 m11 分量，是个近似值。
    qreal  zoom() const;
    void   setZoom(qreal factor, const QPointF &anchorWidget = QPointF());
    void   setZoomAt(qreal factor, const QPointF &anchorWidget);
    void   setZoomLevel(qreal level);          // 对齐到预设级别

    // 平移量（图像左上角在控件坐标中的位置）
    QPointF offset() const;
    void    setOffset(const QPointF &offset);
    void    panBy(const QPointF &deltaWidget);

    // 视图旋转与翻转（只改显示，不动图像数据）
    void setViewRotation(qreal degrees);
    qreal viewRotation() const { return m_viewRotation; }
    void setViewFlipped(bool horizontal, bool vertical);
    bool isViewFlippedH() const { return m_viewFlipH; }
    bool isViewFlippedV() const { return m_viewFlipV; }

    // ── 缩放操作 ──

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToActualSize();

    // 缩放到指定的图像区域（适合窗口 + 指定矩形的组合，裁剪工具会用到）
    void zoomToRect(const QRectF &imageRect);

    // 可视区域（图像坐标）。导航器面板与滚动条用它。
    QRectF visibleImageRect() const;

    // 把指定图像矩形滚动到可视范围内（拖拽到边缘自动滚动用）
    void ensureImageRectVisible(const QRectF &imageRect);

    // ── 坐标换算（工具与界面层的唯一入口）──

    QPointF widgetToImage(const QPointF &widgetPt) const;
    QPointF imageToWidget(const QPointF &imagePt) const;

    // 整数版本，方便不需要亚像素精度的调用方
    QPoint  widgetToImage(const QPoint &widgetPt) const;
    QPoint  imageToWidget(const QPoint &imagePt) const;

    // 矩形换算。注意：做过旋转后返回的是**外接矩形**，不是旋转后的矩形。
    QRectF widgetToImage(const QRectF &widgetRect) const;
    QRectF imageToWidget(const QRectF &imageRect) const;

    // ── 调整/变换预览 ──
    //
    // 设置"临时呈现图"：非空时 paintEvent 画它，而不是 Document 的合成结果。
    //
    // ★ 为什么预览不进 Document ★
    //   Document 是唯一数据源（架构铁律 2）。把"预览"写进文档意味着
    //   它会被算进"是否有未保存修改"、会被别的面板看到、还要在取消时
    //   精确还原 —— 全是麻烦，而且一旦还原逻辑有漏，用户就会带着
    //   一个假的中间结果保存文件。
    //   放在呈现层则天然安全：clearPreviewImage() 一调用，画面立刻回到
    //   文档的真实状态，"取消"不需要任何还原代码。
    //
    // ★ 为什么是降采样图 ★
    //   调整对话框拖动参数时，若按 4000×3000 全尺寸运算，每帧要几十到
    //   上百毫秒，滑块会明显滞后。降采样到最长边 1024（约 100 万像素）
    //   后单帧只要 6~10ms，正好落在 60fps 的预算内
    //   （见 docs/曲线调整设计.md 5.4 的性能分解）。
    //   代价是放大看时预览略糊 —— 而调色看的是整体明暗与色彩倾向，
    //   这个代价换来的跟手程度是值得的。
    //
    // image 为空即清除预览。传非空图时，它会按比例拉伸回**呈现画布**的
    // 尺寸呈现（因此不必关心预览图与画布的实际尺寸比）。
    void setPreviewImage(const QImage &image);

    // ★ 会改变画布尺寸的操作（任意角度旋转）用的重载 ★
    //
    // canvasSize —— 这次预览**将要产生的**画布尺寸。空则等同于文档尺寸。
    //
    //   为什么需要它：旋转后画布按包围盒扩大，来源图与画布不再是同一个
    //   尺寸。若仍按文档的 canvasSize 呈现，用户看到的是"旧取景框里塞进
    //   一个旋转过的图"——放大出来的四角会被取景框裁掉，而真实结果里
    //   那四角是**填充区**。传了新尺寸，棋盘格/边框/网格/可见区域都会
    //   跟着用新尺寸，画面与"确定"之后的样子一致。
    //
    // ★ 预览画布尺寸变化时视图会自动"适合窗口" ★
    //   否则画布突然变大，旋转出来的四角落在窗口之外，用户看不到全貌，
    //   会以为旋转把内容裁掉了。
    void setPreviewImage(const QImage &image, const QSize &canvasSize);

    void clearPreviewImage();
    bool hasPreviewImage() const { return !m_previewImage.isNull(); }

    // ── 辅助显示 ──

    void setGridVisible(bool visible);
    void setGridSize(int pixels);
    bool isGridVisible() const { return m_gridVisible; }

    // 网格间距 / 颜色的只读访问（供"网格间距"菜单与自检读回当前值）
    int    gridSize() const { return m_gridSize; }
    QColor gridColor() const;

    // 网格线颜色。默认是 (120,120,120,110) —— 与 drawGrid 里原来的硬编码一致。
    void setGridColor(const QColor &color);

    // ── 「对齐到网格」──
    //
    // ★ 与「显示网格」是两个独立开关 ★
    //   显示是"我想看见它"，吸附是"我想被它约束"。两者常常一起用，
    //   但"看着网格手工对齐"与"开着吸附看不见网格"都是合理用法，
    //   合成一个开关会逼用户二选一。
    bool snapToGrid() const { return m_snapToGrid; }
    void setSnapToGrid(bool enabled);

    // 吸附容差，**控件（屏幕）像素**。默认 8。
    //   折算成图像像素这一步在 makeToolEvent() 里做（见下）。
    static constexpr qreal kSnapToleranceScreenPx = 8.0;
    qreal snapToleranceScreenPx() const { return m_snapToleranceScreenPx; }
    void  setSnapToleranceScreenPx(qreal px);

    void setCheckerboardVisible(bool visible);
    bool isCheckerboardVisible() const { return m_checkerboardVisible; }

    // ── 参考线（视图辅助，不进合成）──
    //
    // ★ 为什么参考线归画布、不归 Document ★
    //   Document 是"要保存的东西"，参考线是"看图的辅助"。进了 Document 就会
    //   被算进"是否有未保存修改"、被写进撤销栈、被画进导出图 —— 全是麻烦。
    //   放画布里则天然满足：每页一份、导出不可见、撤销栈完全不知道它。
    //
    // ★ 为什么"显示/锁定"是全局、而具体参考线是每页 ★
    //   显示与锁定是**用户偏好**（一次设定、长期有效），所以他俩放 MainWindow
    //   的记忆里、切页时补推给画布；而哪几根线是当前这张图的视图状态，随页走。

    bool guidesVisible() const { return m_guidesVisible; }
    void setGuidesVisible(bool visible);

    // 全局锁：锁住后所有参考线都不能拖动/删除（单根锁见 Guide::locked）。
    bool guidesLocked() const { return m_guidesLocked; }
    void setGuidesLocked(bool locked);

    const QVector<Guide> &guides() const { return m_guides; }

    int  addGuide(Qt::Orientation orientation, int posImage);   // 返回新索引
    void moveGuide(int index, int posImage);
    void removeGuide(int index);
    void clearGuides();
    void setGuideLocked(int index, bool locked);

    // 从标尺拖拽过程中那根"还没落下的"参考线。orientation 是**参考线**的方向。
    void setPendingGuide(Qt::Orientation orientation, int posImage);
    void clearPendingGuide();
    bool hasPendingGuide() const { return m_hasPendingGuide; }
    Guide pendingGuide() const { return m_pendingGuide; }

    // 命中测试：控件坐标附近有没有**可拖动**的参考线。有则返回索引，否则 -1。
    // tolPx 用**控件像素**（与"框选阈值用控件坐标"同一条纪律，见铁律第十四章）。
    int guideAtWidget(const QPointF &widgetPos, qreal tolPx) const;

    // 当前光标位置的图像坐标（供状态栏显示）
    QPointF cursorImagePos() const { return m_cursorImagePos; }

    // ── 临时状态 ──

    // 手动指定光标（工具会调用它来显示笔刷轮廓等）
    void refreshCursor();

    // 请求取消当前工具操作（Esc）
    void cancelActiveTool();

    // 强制重绘（工具数据变更后调用）
    void invalidateComposite();

signals:
    // 视图状态变化（缩放/平移/旋转）。状态栏、导航器、标尺都监听它。
    void viewChanged();

    // 光标位置变化（图像坐标）
    void cursorMoved(const QPointF &imagePos);

    // 光标下方的像素颜色（状态栏的 RGB 取样色块用）
    void cursorColorSampled(const QColor &color, const QPointF &imagePos);

    // 工具发出的状态消息
    void statusMessage(const QString &message, int timeoutMs);

    // 工具请求缩放（缩放工具用信号而不是直接改视图，
    // 因为"缩放多少"是视图层的策略，工具只表达"用户想放大"）。
    //
    // ★ 锚点是控件坐标 ★ —— 见 AbstractTool::requestZoom 的说明。
    //   本类在构造函数里把它接回 setZoomAt()（默认行为），
    //   将来若有人想在 MainWindow 侧旁听"用户用缩放工具缩放了"，
    //   连这个信号即可，不必改工具。
    void zoomRequested(qreal factor, const QPointF &anchorWidget);

protected:
    void paintEvent(QPaintEvent *event) override;

    /// 主题切换要重取棋盘格颜色（它跟深浅走），见 drawCheckerboard。
    void changeEvent(QEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    void wheelEvent(QWheelEvent *event) override;

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

    // 文字浮层的事件过滤器（Esc / Ctrl+Enter / 失焦）。
    //   ★ 为什么用过滤器而不是给 QTextEdit 派生一个子类 ★
    //     浮层的全部行为都只有三件事（确认 / 取消 / 跟随视图），
    //     为它派生一个类意味着"这个类属于谁"的问题（ui 还是 tools？），
    //     而它其实只是画布的一个临时子控件。过滤器把这三条规则留在
    //     唯一需要它们的那个类里，不新增类型。
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // 文档内容变化 —— 失效合成缓存并重绘
    void onDocumentContentChanged();
    void onDocumentStructureChanged();
    void onCanvasSizeChanged(const QSize &size);

    // 自动滚动（拖拽到边缘时）
    void onAutoScrollTimeout();

    // 蚂蚁线的动画节拍：推进虚线相位并请求重绘。
    // ★ 只在"存在选区"时才运行 ★ 见 updateAntsTimer() 的说明。
    void onAntsTick();

private:
    // ── 呈现管线的各个步骤 ──

    // 重建视图变换矩阵。
    // ★ 所有视图状态（缩放、平移、旋转、翻转）都通过这一个函数
    //   合成到 m_viewTransform 里 ★
    //   好处：不存在"矩阵与标量不同步"的可能 —— 那种 bug 表现为
    //   "画面显示是对的，但点击位置偏了"，极难排查。
    void rebuildViewTransform();

    // ★ 呈现用的画布尺寸 —— 全类只有这一个取尺寸的口径 ★
    //
    //   预览期间若预览自带画布尺寸（旋转），就用它；否则用文档的。
    //
    //   为什么必须收敛成一个函数：画布尺寸同时参与**视图变换的旋转/翻转
    //   中心**、fitZoom、可见区域求交、棋盘格、边框、网格。只要有一处
    //   用了文档尺寸、另一处用了预览尺寸，就会出现"取景框对、图像偏"
    //   这类只在预览期间复现的错位（铁律八：坐标口径必须唯一）。
    QSize effectiveCanvasSize() const;

    // 计算"适合窗口"的缩放比
    qreal fitZoom() const;

    // 绘制步骤
    void drawBackground(QPainter &painter, const QRect &widgetRect);
    void drawCheckerboard(QPainter &painter, const QRectF &imageRect);
    void drawComposite(QPainter &painter, const QRectF &visibleImageRect);
    void drawGrid(QPainter &painter, const QRectF &visibleImageRect);
    // 第 7 步：参考线（纯视图辅助，**绝不进入合成**）
    void drawGuides(QPainter &painter, const QRectF &visibleImageRect);
    void drawBorder(QPainter &painter);
    // 第 9 步：选区蚂蚁线（图像坐标，但线宽恒定于屏幕）
    void drawSelectionAnts(QPainter &painter, const QRectF &visibleImageRect);
    // 第 8 步：工具叠加层（图像坐标）
    void drawToolOverlay(QPainter &painter);
    // 第 10 步：工具叠加层（控件坐标，屏幕尺寸恒定）
    void drawToolScreenOverlay(QPainter &painter);

    // ── 选区轮廓缓存 ──

    // 重建缓存：算法本体在 engine/outline::maskOutline（纯函数），
    // 这里只负责"喂哪些输入 + 缓存到哪 + 记账"。
    void rebuildSelectionOutline();

    // ★ 需不需要重建：只看轮廓的**全部输入**有没有变 ★
    //
    //   输入① 选区蒙版  —— 用 Selection::revision() 代表
    //   输入② 画布尺寸
    //
    //   ★ 这条判据是"每个画笔事件重扫一遍选区轮廓"的终结者 ★
    //     （评估文档 §1.3）。画笔改的是图层像素，与选区蒙版无关，
    //     所以那笔开销原来是纯白花的。
    //
    //   ★ 为什么不用"比较蒙版内容" ★
    //     比较内容要遍历整张蒙版 —— 那正是要省掉的开销本身。
    //     所以 Selection 必须能回答"我变了没有"（revision）。
    bool outlineNeedsRebuild() const;

    // 强制失效（不改输入、但语义变了的场合：切文档 / 结构变化）。
    // ★ 别拿它当万能刷新用 ★ 它一定会导致下一次绘制重建整条轮廓。
    void invalidateSelectionOutline();

    // 根据"是否存在选区"决定要不要跑蚂蚁线动画定时器。
    //
    // ★ 必须随选区状态开关 ★
    //   动画意味着每 120ms 触发一次整块画布重绘。如果无条件常开，
    //   用户即使完全没有用选区，CPU 也会持续空转 —— 在笔记本上
    //   直接影响续航。没有选区时精确地停掉它。
    void updateAntsTimer();

    // ── 输入辅助 ──

    // 从 Qt 事件构造 ToolEvent。
    // ★ 这里是 Qt5 / Qt6 差异的收敛点 ★
    //   QMouseEvent::localPos() 在 Qt6 被删除，position() 在 Qt5 不存在。
    //   通过 compat::eventPos() 模板统一处理，工具层因此看不到任何 #if。
    ToolEvent makeToolEvent(QMouseEvent *event, ToolEventType type) const;

    // 把工具的 ToolResult 转换成实际的视图动作（重绘等）
    void         handleToolResult(const ToolResult &result, const ToolEvent &event);

    // 是否应当把事件交给工具（有一些前置拦截）
    bool shouldDispatchToTool(QMouseEvent *event) const;

    // 取样光标下的颜色（状态栏显示用）
    void sampleCursorColor(const QPointF &imagePos);

    // ── 文字输入浮层 ──
    //
    //  ★ 浮层为什么归画布、而不是归 TextTool ★
    //    它必须挂在画布上才能定位到"用户点击的那一点"，而 tools 层不允许
    //    依赖 ui 层。于是分工是：工具管**内容**（发 requestTextEditor、
    //    收 setEditingText、commit 时落像素），画布管**这个控件**。
    //    两端通过 AbstractTool 上的那条协议牵线，画布**不认识 TextTool**。
    //
    //  openTextEditor  在图像坐标 imageAnchor 处开一个浮层。
    //                  font 是**图像像素**字号，画布负责乘上 zoom 显示。
    //                  align 是 TextAlign 的整数值（避免本头文件依赖 ToolParams.h），
    //                  lineHeight 是行距倍数（QFont 里没有这个概念，只能单独传）。
    void openTextEditor(const QPointF &imageAnchor, const QFont &font,
                        const QColor &color, int align, qreal lineHeight);

    //  closeTextEditor  收掉浮层。commit 为 true 时先把文本交给工具提交。
    //
    //  ★ 为什么只有一个函数带这个 bool，而不是 open/commit/cancel 三个 ★
    //    "收浮层"这件事必须**幂等且只有一个出口**：Esc、失焦、换工具、
    //    换文档四条路径都会走到这里。写成多个入口，早晚会有一条路径漏掉
    //    "把指针置空"，于是浮层被删了指针还指着 → 悬空。
    void closeTextEditor(bool commit);

    //  让浮层跟随视图（缩放/平移后重新摆位、重算显示字号）。
    //  没有浮层时什么都不做。
    void updateTextEditorLayout();

    // ── 状态 ──

    Document     *m_doc        = nullptr;
    AbstractTool *m_activeTool = nullptr;

    // ★ 唯一的视图状态 ★
    //   图像坐标 → 控件坐标。
    //   与 m_viewRotation / m_viewFlipH/V 的关系：
    //   后三者是"用户意图"的记录，m_viewTransform 是它们的**合成结果**。
    //   为什么不把旋转/翻转也塞进矩阵而只保留一个矩阵？
    //     因为"再旋转 15 度"需要知道当前累计角度，从矩阵反解角度
    //     涉及 atan2 与奇异点判断（90 度附近不稳定）。
    //     分开存更可靠，代价是 rebuildViewTransform() 里多几行合成代码。
    QTransform m_viewTransform;
    qreal      m_viewRotation = 0.0;
    bool       m_viewFlipH    = false;
    bool       m_viewFlipV    = false;

    // 缩放与平移的"逻辑值"。
    // 为什么要单独存而不是从矩阵读：
    //   矩阵里的 m11 在旋转 45 度后不再等于缩放比（被 cos45 污染）。
    //   保留逻辑值是让 setZoom 类接口有稳定的语义。
    qreal  m_zoom   = 1.0;
    QPointF m_offset;

    // "视图尚未被用户调整过"。
    //
    // ★ 为什么需要这个标志 ★
    //   主窗口的构造顺序是"先建文档 → 再 show()"，而画布的真实尺寸要等
    //   布局完成才有。setDocument() 里的 zoomToFit() 因此可能按 QWidget
    //   的默认尺寸（640×480）算缩放 —— 3840×2160 的照片会被算成 16%，
    //   窗口真正显示出来后画面只占一小块，而且此后不再自我纠正。
    //
    //   只判断一次"控件拿到尺寸了没"是不可靠的：show() 自己也会派发一次
    //   resize（尺寸仍是默认值），会把那次机会提前消耗掉。
    //
    //   所以改用这条规则：**只要用户还没亲手动过视图，尺寸一变就重新适合窗口**。
    //   用户一旦缩放/平移过（哪怕只是滚了一格滚轮），标志置位，
    //   之后再改变窗口大小就只保持中心不动、不再改动缩放 ——
    //   不会跟用户的意图打架。
    //
    //   打开新文档时该标志复位，于是新文档又回到"自动适合窗口"状态。
    bool m_userAdjustedView = false;

    // 合成结果缓存。
    // 只在文档内容变化时失效 —— 缩放平移**不**重算合成，
    // 只改变 drawImage 的目标矩形。这是大图流畅缩放的关键。
    QImage m_compositeCache;
    bool   m_compositeDirty = true;

    // 临时呈现图（调整对话框的降采样预览 / 旋转对话框的旋转预览）。
    // 非空时第 5 步画它而不是 m_compositeCache —— 它是"整幅图的缩小版"，
    // 绘制时按比例拉伸回画布尺寸，因此不需要额外记录缩放比。
    QImage m_previewImage;

    // 预览图对应的画布尺寸。空表示"与文档一致"（调整类预览）。
    // 非空时全类以它作为呈现画布尺寸（见 effectiveCanvasSize()）。
    QSize  m_previewCanvasSize;

    // 光标位置
    QPointF m_cursorImagePos;
    bool    m_hasCursor = false;

    // 颜色
    QColor m_foregroundColor = Qt::black;
    QColor m_backgroundColor = Qt::white;

    // ── 交互状态 ──

    // 空格平移
    bool    m_spacePressed = false;
    bool    m_panning      = false;
    QPointF m_panStartWidgetPos;
    QPointF m_panStartOffset;

    // 中键平移
    bool m_middlePanning = false;

    // 滚轮累积（高精度滚轮会发很多小 delta，累积到一定量才缩放一次，
    // 否则触控板的惯性滚动会让缩放疯狂跳动）
    int m_wheelAccumulator = 0;

    // ── 自动滚动 ──

    QTimer *m_autoScrollTimer = nullptr;
    QPointF m_autoScrollDirection;      // 归一化方向，例如 (1, 0) 表示向右滚

    // ── 显示选项 ──

    bool m_gridVisible          = false;
    int  m_gridSize             = 64;      // 网格间距（图像像素）
    QColor m_gridColor          = QColor(120, 120, 120, 110);  // 网格线颜色（原来硬编码在 drawGrid 里）
    bool m_snapToGrid           = false;   // 「对齐到网格」（与显示独立）
    qreal m_snapToleranceScreenPx = kSnapToleranceScreenPx;   // 吸附容差（屏幕像素）
    bool m_checkerboardVisible  = true;
    int  m_checkerboardSize     = 12;      // 棋盘格边长（控件像素，屏幕尺寸恒定）

    // ── 参考线 ──

    QVector<Guide> m_guides;              // 每页一份（本控件就是"一页"）
    bool           m_guidesVisible = true;
    bool           m_guidesLocked  = false;

    // 从标尺拖出来、还没落下的那一根。
    //   ★ 必须单独存、不能"直接 addGuide 再删掉" ★
    //     直接加进去会让"是否已提交"无从判断，松手时又得按位置反查是哪一根 ——
    //     而两根参考线可能刚好重合。分开存则语义唯一：pending 就是"还没落下的"。
    bool  m_hasPendingGuide = false;
    Guide m_pendingGuide;

    // 画布侧正在拖动的参考线索引（-1 = 没在拖）。
    // ★ 用索引而不是拷贝 ★ 拖动过程中要不断 moveGuide()，
    //   拷贝一份就会出现"拖的是那个副本、列表里那根不动"。
    int  m_draggingGuide  = -1;

    // 滚轮行为：true = 缩放，false = 垂直滚动
    bool m_wheelZooms = true;

    // ── 选区蚂蚁线 ──

    // 选区边界的轮廓路径（图像坐标）。
    // 由 rebuildSelectionOutline() 从灰度蒙版提取，选区不变时就一直是这份。
    QPainterPath m_selectionOutline;

    // 缓存的有效性 = "重建时所依据的两个输入"
    //   ★ 记的是**输入**，不是"重建过了"这个事件 ★
    //     记事件的话就没法回答"现在这份缓存还算不算数"。
    quint64 m_outlineSelectionRevision = 0;
    QSize   m_outlineCanvasSize;
    // 强制失效标志（切文档 / 结构变化 / 画布尺寸变化）
    bool    m_selectionOutlineDirty = true;
    // 遥测：真的重建了几次（见 outlineRebuildCount()）
    quint64 m_outlineRebuildCount = 0;

    // 蚂蚁线的虚线相位。每次定时器触发加 1，让虚线段"流动"起来 ——
    // 这是"蚂蚁线"这个叫法的由来，也是让用户一眼看出
    // "这是一条会动的选区边界，而不是图像上本来的一条线"的关键。
    int          m_antsPhase = 0;

    // 蚂蚁线动画定时器。惰性创建（第一次需要时才 new），因为
    // 大多数会话里用户可能一直不做选区。
    QTimer      *m_antsTimer = nullptr;

    // 性能计时（用于判断是否需要降采样预览）
    mutable QElapsedTimer m_lastPaintTimer;

    // ── 文字输入浮层 ──

    // 浮层控件（属于本控件）。nullptr 表示当前没有在输入文字。
    // 生命周期由 closeTextEditor() 唯一管理（deleteLater）。
    QTextEdit *m_textEditor = nullptr;

    // 正在关闭浮层（防止"hide/析构引发的又一次 FocusOut"递归进来）。
    // ★ 这类重入是界面代码里最常见的隐蔽崩溃来源 ★
    //   closeTextEditor 会 deleteLater，而析构前 Qt 还会派发 FocusOut；
    //   没有这个标志的话，第二次进入时会对着一个已经在关闭过程中的
    //   控件再调一次 commit —— 表现是"一次确认压了两条撤销命令"。
    bool m_closingTextEditor = false;

    // 浮层当前的锚点（图像坐标）。视图变化时用它重新摆位。
    QPointF m_textEditorAnchor;

    // 浮层用的字体，**字号是图像像素**（不是屏幕像素）。
    // 显示时按 zoom 乘一次（见 updateTextEditorLayout）。
    // ★ 必须保存"图像字号"而不是直接把缩放后的字体塞进控件 ★
    //   否则每次缩放都要在"上一次的结果"上再乘一次 —— 滚两格滚轮
    //   字号就变成 1.25² ≈ 1.56 倍，越滚越大（这是"在结果上累加"
    //   而不是"从源头重算"，与形状工具拖动预览那条纪律同源）。
    QFont   m_textEditorFont;

    int     m_textEditorAlign = 0;
    qreal   m_textEditorLineHeight = 1.2;
};

} // namespace zl
