#pragma once

// ============================================================================
//  LayerPanel.h —— 图层面板（阶段五的核心交付）
// ----------------------------------------------------------------------------
//  为什么给它单独一个类，而不是继续堆在 MainWindowActions.cpp 里：
//
//    ① MainWindowActions.cpp 已经一千多行。图层面板自带一整套状态 ——
//       行序映射、热点命中、属性控件回填抑制、缩略图节流 —— 塞进去会把
//       那个文件的阅读顺序彻底打乱。
//    ② 面板必须**能单独测**。面板里最容易写错的两件事都是"没有编译期
//       保护、跑起来才看得见"的那类：
//         · 视图行序 ⇄ 文档索引序（文档索引 0 在最底，面板第一行在最顶）
//         · 行内热点的命中（眼睛 / 锁到底点没点中）
//       所以这两件事被抽成**不依赖任何控件的纯函数**（namespace layerpanel），
//       可以不起窗口、直接断言。见 tests/layer_panel_test.cpp。
//
//  ★ 面板不改内容 ★
//    面板只允许**读** Document，以及写一个例外：活动图层索引
//    （`setActiveLayerIndex` 是纯 UI 状态，不进撤销栈，与旧的最小版本一致）。
//    其余一切改动图层内容或元数据的操作都**发信号**出去，由 MainWindow
//    包装成 LayerCommand 压栈。这样：
//      · 撤销/重做只有一条路径，不可能漏
//      · 面板可以在没有 CommandStack 的情况下单独构造（自检需要）
//
//  ★ 视图行序 ⇄ 文档索引序 ★
//    Document 的索引 0 = **最底部**图层，索引越大越靠上；
//    面板的第一行 = **最顶部**图层。所以 row = count - 1 - index。
//    忘了倒序的表现是："上移图层"在界面上看着往下走 —— 不报错、不崩溃，
//    只是反了。全工程只有 layerpanel::rowToLayerIndex/layerIndexToRow
//    两个地方允许做这个换算。
// ============================================================================

#include "core/ImageTypes.h"

#include <QList>
#include <QListWidget>
#include <QPair>
#include <QStyledItemDelegate>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace zl {

class Document;

// ============================================================================
//  纯函数区：不依赖控件，可直接断言
// ============================================================================
namespace layerpanel {

// ── 行的几何常量 ──
//  抽出来的理由：绘制（delegate）与命中判定（hotspotAt）必须用**同一套**
//  数字。分开写两份的话，改一个忘一个，症状是"眼睛图标画在这里、点了没反应"。
constexpr int kRowHeight = 46;
constexpr int kThumbSize = 34;
constexpr int kThumbLeft = 5;
constexpr int kHitWidth  = 22;   // 眼睛 / 锁 的点击区宽度（正方形）

/// 视图行 → 文档图层索引。row 越界时返回 -1。
int rowToLayerIndex(int row, int layerCount);

/// 文档图层索引 → 视图行。index 越界时返回 -1。
int layerIndexToRow(int layerIndex, int layerCount);

/// 批量换算（多选时用）。保持输入顺序，丢弃越界项。
QList<int> rowsToLayerIndices(const QList<int> &rows, int layerCount);

/// 缩略图矩形（行内坐标，相对 rowRect 左上角）
QRect thumbnailRect(const QRect &rowRect);
/// 眼睛的点击矩形
QRect eyeRect(const QRect &rowRect);
/// 锁的点击矩形
QRect lockRect(const QRect &rowRect);
/// 名字（+ 副标题）的矩形
QRect nameRect(const QRect &rowRect);

/// 行内热点
enum class Hotspot {
    Outside,    // 不在这一行里
    Thumbnail,  // 缩略图 / 名字区 —— 拖动、双击重命名、切活动图层都走这里
    Visible,    // 眼睛
    Lock        // 锁
};

/// 判断 pos（**控件坐标**）落在行的哪个热点上。
/// ★ 眼睛与锁必须在缩略图/名字区**之前**判定 ★
///   它们的矩形与名字区在水平方向上有重叠的边界，判定顺序决定了"点眼睛
///   到底算不算点眼睛"。这里从右往左判，让可点击的小目标优先。
Hotspot hotspotAt(const QRect &rowRect, const QPoint &pos);

/// 拖拽落点 → `Document::moveLayer(from, to)` 的 `to`。
///
///  参数全在**文档索引**空间：
///    srcIndex      被拖动的图层索引
///    dropBeforeRow 拖放指示线所在的**视图行**（插到该行之前，取值 0..count）
///
///  ★ 两个坑 ★
///    ① `QList::move(from, to)` 的 to 是"**取出 from 之后**的坐标"，
///       所以源项原本在落点之上时，插入位置要往前挪一位。
///    ② 行序是倒的，所以最后还得换回文档索引。
///  返回值越界时返回 -1（调用方应直接跳过这次移动）。
int dropRowToTargetIndex(int srcIndex, int dropBeforeRow, int layerCount);

} // namespace layerpanel

// ============================================================================
//  行绘制
// ============================================================================
//
//  用 delegate 自绘而不是 setItemWidget 塞控件：
//    · setItemWidget 的项**不能拖拽排序**（QListWidget 的 InternalMove 会
//      在拖动时销毁/重建项，子控件随之丢失），这是最直接的否决理由
//    · 一屏几十行时，自绘的开销远小于几十个 QWidget 的布局开销
//  代价是"眼睛/锁点击区"要自己算 —— 这正是上面 hotspotAt 存在的理由。

class LayerItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit LayerItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    /// 编辑器只盖住"名字"那一块，缩略图与眼睛/锁保持可见 ——
    /// 否则编辑时看不出来正在改哪一行。
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;

    /// ★ 刻意**不把新名字写回 model** ★
    ///   行的显示内容来自 Document。写回 model 就有了两份真相：
    ///   撤销一次改名后，行上显示的还是新名字（没人再去擦一次 model）。
    ///   这里只把意图发出去，等文档变 → structureChanged → 重建行。
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override;

signals:
    void commitRename(int row, const QString &name);
};

// ============================================================================
//  列表控件：负责行序、多选、拖拽、双击重命名、热点点击
// ============================================================================

class LayerListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit LayerListWidget(QWidget *parent = nullptr);

    /// 进入某一行的重命名编辑态（菜单/快捷键也能触发）
    void beginRename(int row);

signals:
    void visibilityToggled(int layerIndex, bool visible);
    void lockToggled(int layerIndex, bool locked);

    /// 拖动排序：from/to 都是**文档索引**
    void reorderRequested(int fromIndex, int toIndex);

    /// 有文件被拖到列表上（而不是图层行被拖动）。
    /// 面板自己不打开文件，原样转给 MainWindow。
    void filesDropped(const QStringList &paths);

protected:
    // 外部文件拖放。★ 必须在这里接住，否则文件永远打不开 ★
    //   Qt 找拖放目标时从鼠标下最内层控件往上找第一个 acceptDrops() 的控件，
    //   找到就只发给它 —— 它在 dropEvent 里 ignore() 也不会回退给父控件。
    //   本控件为了拖拽排序必须是 acceptDrops(true)，于是拖到图层面板上的
    //   文件会被它吞掉（画布那边是同一个坑，修法相反：那边根本不该 accept）。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    /// 按下鼠标的那一行。
    /// 为什么需要它：ExtendedSelection 下"被拖动的行"不一定是 currentItem
    /// （用户可以先框选多行、再从中间某一行的**图标区之外**开始拖）。
    /// 拖放结束时用"按下的那一行"当源，才和用户的手一致。
    int m_pressRow = -1;
};

// ============================================================================
//  图层面板
// ============================================================================

class LayerPanel : public QWidget
{
    Q_OBJECT

public:
    /// thumbnailThrottleMs —— 缩略图刷新节流窗口。
    ///   默认 300ms（见 .cpp 里为什么**必须**节流）。自检传 0 让它同步生效。
    explicit LayerPanel(int thumbnailThrottleMs = 300, QWidget *parent = nullptr);

    void setDocument(Document *doc);
    Document *document() const { return m_doc; }

    /// 当前选中的图层索引（文档索引序，升序）
    QList<int> selectedLayerIndices() const;

    /// 让活动图层那一行进入重命名编辑态
    void beginRenameActiveLayer();

    // ── 下面这些是给自检用的观测口，应用逻辑不该依赖它们 ──

    QListWidget *listWidget() const { return m_list; }
    QComboBox   *blendCombo() const { return m_blendCombo; }
    QSlider     *opacitySlider() const { return m_opacitySlider; }
    QList<QToolButton *> buttons() const { return m_buttons; }

    /// 实际生成过多少次图层缩略图。
    /// 断言"节流生效"就靠它：连打 20 次 contentChanged，这个数必须纹丝不动。
    int  thumbnailRenderCount() const { return m_thumbnailRenderCount; }
    int  throttleIntervalMs() const;
    void setThrottleIntervalMs(int ms);
    bool throttlePending() const;

    /// 立刻刷一遍缩略图（跳过节流）。自检与"手动刷新"用。
    void refreshThumbnailsNow();

    /// 按当前 palette 重新取一遍本面板里的图标（换主题后由 MainWindow 调）。
    /// ★ 图标是渲染时刻定色的 ★ 只清图标缓存不会改动已经设上去的 QIcon。
    void refreshIcons();

signals:
    // ── 结构类（MainWindow 包装成 LayerCommand）──
    void addLayerRequested();
    void duplicateRequested(const QList<int> &layerIndices);
    void deleteRequested(const QList<int> &layerIndices);
    void moveUpRequested();
    void moveDownRequested();
    void mergeDownRequested(int layerIndex);
    void mergeVisibleRequested();
    void flattenRequested();
    void reorderRequested(int fromIndex, int toIndex);

    /// 文件被拖到图层面板上。面板负责转达，打开文件是 MainWindow 的事。
    void filesDropped(const QStringList &paths);

    // ── 属性类 ──
    void visibilityToggled(int layerIndex, bool visible);
    void lockToggled(int layerIndex, bool locked);
    void renameRequested(int layerIndex, const QString &name);

    /// 混合模式：一次点击就是一次完整改动，直接压一条命令
    void blendModeChanged(int layerIndex, int blendBefore, int blendAfter);

    /// 不透明度拖动中：**只改内存、不压栈**（等同于其它对话框的"实时预览"）。
    /// 不这么做的话，拖一次滑块会往撤销栈里塞几十条命令。
    void opacityPreviewed(int layerIndex, int opacity);
    /// 不透明度松手：压**一条**命令
    void opacityCommitted(int layerIndex, int before, int after);

public slots:
    /// 文档结构/属性变化后重建列表（由 Document::structureChanged 驱动）
    void rebuildRows();

private:
    void setupUi();
    void bindDocument(Document *doc);
    void syncSelectionToActiveLayer();
    void syncControlsToActiveLayer();
    void scheduleThumbnailRefresh();
    void updateRowDisplay(int layerIndex);
    void updateCountLabel();

    Document        *m_doc = nullptr;
    LayerListWidget *m_list = nullptr;
    QComboBox       *m_blendCombo = nullptr;
    QSlider         *m_opacitySlider = nullptr;
    QLabel          *m_opacityLabel = nullptr;
    QLabel          *m_countLabel = nullptr;
    QList<QToolButton *> m_buttons;
    /// 属性区那两个"图标 + 提示"的标签。换主题时按名字重新上色需要用。
    QList<QPair<QLabel *, QString>> m_iconLabels;
    QTimer          *m_thumbTimer = nullptr;

    int  m_thumbnailRenderCount = 0;
    int  m_opacityAtPress = -1;      // 按下滑块时的值，松手时作为 before
    bool m_syncing = false;          // 回填抑制：防止"改控件 → 触发信号 → 又改文档"
};

} // namespace zl
