// ============================================================================
//  layer_panel_test.cpp —— 图层面板的离线自检
// ----------------------------------------------------------------------------
//  图层面板里最容易写错的东西有**同一个特征**：没有编译期保护，
//  跑起来也不报错，而且大部分肉眼看不出问题。所以这组测试专门盯它们：
//
//    ① 视图行序 ⇄ 文档索引序
//       Document 的索引 0 是**最底部**图层，面板第一行是**最顶部**图层。
//       忘掉倒序的症状是"上移图层在界面上看着往下走" —— 不报错、不崩溃，
//       只是反了。这一条是本文件里最重要的不变量。
//
//    ② 行内热点的命中（眼睛 / 锁）
//       自绘行没有控件级的命中测试，"眼睛画在这里、点了没反应"只能靠
//       "绘制与命中共用同一套矩形"来避免。这里断言它们本来就分开。
//
//    ③ 拖拽落点 → 目标索引
//       涉及两次坐标换算（视图行 ⇄ 文档索引）+ 一次 QList::move 的
//       "取出后坐标"补偿，off-by-one 有四种写法，其中三种看起来都对。
//       做法：纯函数的输出 → 真的喂给 Document::moveLayer → 与语义模型对拍。
//
//    ④ 面板只发信号、自己绝不改文档
//       面板若直接改 Document，就会出现"点眼睛能隐藏、但 Ctrl+Z 没反应"。
//       断言是**反向**的：点击后信号要发、而文档必须**没变**。
//
//    ⑤ 自绘行的像素
//       见「实现铁律」：每条自绘管线都要有一条"量它画出来的像素"的断言。
//       断言数学全绿而画面不对，是这类代码最典型的失败形态 ——
//       所以这里真的 grab() 列表控件，去数量出来的像素。
//
//    ⑥ 缩略图节流
//       不节流的话，画笔每画一笔都重建整个列表 —— 图层多时会明显卡顿，
//       而且正在重命名的编辑框会被冲掉。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 layer_panel 分支。需要 QApplication（控件与 QPixmap）
//    与 Qt5Svg（行上的眼睛/锁小图标走 IconLibrary），用 offscreen 平台插件跑。
// ============================================================================

#include "ui/LayerPanel.h"

#include "core/Document.h"
#include "core/Layer.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QListWidget>
#include <QMouseEvent>
#include <QPixmap>
#include <QRect>
#include <QSlider>
#include <QString>
#include <QStringList>

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

static void checkEqInt(int actual, int expected, const char *what)
{
    if (actual == expected) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    }
}

static void checkEqStr(const QString &actual, const QString &expected,
                       const char *what)
{
    if (actual == expected) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s  期望「%s」，实际「%s」\n", what,
                    qUtf8Printable(expected), qUtf8Printable(actual));
    }
}

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

/// 造一个带指定图层名的文档（names[0] 在最**底**部）
static Document *makeDoc(const QStringList &names)
{
    auto *doc = new Document(QSize(64, 48), Qt::transparent, nullptr);

    // 构造 Document 时会自带一个图层，把它改成 names[0]；
    // 其余的用 addLayer 追加（每个都插在活动图层之上，所以顺序就是参数顺序）。
    doc->layers().mutableAll()[0].setName(names.value(0));

    for (int i = 1; i < names.size(); ++i)
        doc->addLayer(names.at(i));

    doc->setActiveLayerIndex(0);
    return doc;
}

/// 按**视图行序**（上 → 下）取图层名
static QStringList visualNames(const Document *doc)
{
    QStringList out;
    for (int row = 0; row < doc->layerCount(); ++row) {
        const int index = layerpanel::rowToLayerIndex(row, doc->layerCount());
        const Layer *layer = doc->layers().layer(index);
        out << (layer ? layer->name() : QStringLiteral("<空>"));
    }
    return out;
}

/// 语义模型：把 srcIndex 拖到"视图第 dropBeforeRow 行之前"之后，
/// 被拖动的那一项最终落在**视图第几行**。
///
/// ★ 这是**独立于实现**的一份定义 ★
///   它只用到两件事：
///     · 源项被取出后，它下面的插槽整体前移一位（所以插槽要减一）
///     · 落点不能越界
///   而 dropRowToTargetIndex() 是"实现"，额外还要做 视图行 → 文档索引 的换算。
///   两边算出来的东西不同，所以拿它们对拍才有意义。
static int modelFinalInsertRow(int srcIndex, int dropBeforeRow, int count)
{
    const int srcRow = count - 1 - srcIndex;   // 文档索引 → 视图行
    int insertRow = qBound(0, dropBeforeRow, count);
    if (srcRow < insertRow)
        --insertRow;
    return qBound(0, insertRow, count - 1);
}

/// 按语义模型给出"拖完之后应有的视图行序"
static QStringList modelVisualNames(const QStringList &docOrderNames,
                                    int srcIndex, int dropBeforeRow)
{
    QStringList vis;                                  // 视图行序 = 文档序的倒序
    for (int i = docOrderNames.size() - 1; i >= 0; --i)
        vis << docOrderNames.at(i);

    const int srcRow = docOrderNames.size() - 1 - srcIndex;
    const QString moved = vis.takeAt(srcRow);
    vis.insert(modelFinalInsertRow(srcIndex, dropBeforeRow, docOrderNames.size()),
               moved);
    return vis;
}

/// 构造"某一行的行矩形"（与面板的绘制/命中共用同一套纯函数）
static QRect rowRectFor(const QListWidget *list, int row)
{
    const QModelIndex idx = list->model()->index(row, 0);
    return QRect(0, list->visualRect(idx).top(),
                 list->viewport()->width(), layerpanel::kRowHeight);
}

/// 往列表的 viewport 发一次左键按下。
/// 走的是真实事件路径：QAbstractScrollArea 会把 viewport 的鼠标事件
/// 转发给 view 的 mousePressEvent（与用户真点一下完全同一条路）。
static void clickAt(QListWidget *list, const QPoint &pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);
}

/// 抓列表 viewport 上的一块区域（**控件坐标** → 图像坐标，含 dpr 缩放）
static QImage grabRegion(QListWidget *list, const QRect &viewportRect)
{
    const QPixmap shot = list->viewport()->grab();
    const QImage full = shot.toImage().convertToFormat(QImage::Format_ARGB32);
    if (full.isNull())
        return QImage();

    const qreal dpr = list->viewport()->devicePixelRatioF();
    const qreal s = (dpr > 0.0) ? dpr : 1.0;

    QRect device(int(viewportRect.left()   * s), int(viewportRect.top()    * s),
                 int(viewportRect.width()  * s), int(viewportRect.height() * s));
    return full.copy(device);
}

/// 数"偏红"的像素（用来证明缩略图真的把红色图层画出来了）
static int countReddish(const QImage &img)
{
    if (img.isNull())
        return -1;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) > 170 && qGreen(p) < 100 && qBlue(p) < 100)
                ++n;
        }
    }
    return n;
}

/// 数"很暗"的像素（眼睛/锁的描边是 #33393f，用来证明图标真画上去了）
static int countDark(const QImage &img)
{
    if (img.isNull())
        return -1;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) + qGreen(p) + qBlue(p) < 360)
                ++n;
        }
    }
    return n;
}

/// 两张同尺寸图里"明显不同"的像素数
static int diffPixels(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            const int d = qAbs(qRed(p) - qRed(q)) + qAbs(qGreen(p) - qGreen(q))
                        + qAbs(qBlue(p) - qBlue(q));
            if (d > 24)
                ++n;
        }
    }
    return n;
}

/// 造一张纯色图，用来做"缩略图有没有内容"的对照
static QImage solidImage(int w, int h, const QColor &color)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

/// 让面板重建列表（等价于文档发了一次 structureChanged）
///
/// ★ 说明 ★
///   正常情况下这个信号由 Document 自己发（增删图层、LayerCommand 回放）。
///   这里从外面 emit 是为了造出"图层名被改了"这种**没有公开入口**
///   的中间状态 —— Document 没有 setLayerName()，改名走的是
///   LayerCommand::applyProperty()。本测试刻意不链接 commands/，
///   因为面板**不该**依赖命令层（面板只发信号、由 MainWindow 包命令）。
static void pokeStructure(Document *doc)
{
    emit doc->structureChanged();
    QApplication::processEvents();
}

// ===========================================================================
//  [1] 行序映射
// ===========================================================================
static void testRowMapping()
{
    beginGroup("视图行序 ⇄ 文档索引序");

    // 5 个图层：索引 0 在最底，视图第 0 行是最顶
    checkEqInt(layerpanel::rowToLayerIndex(0, 5), 4, "第 0 行 = 最顶（索引 4）");
    checkEqInt(layerpanel::rowToLayerIndex(4, 5), 0, "第 4 行 = 最底（索引 0）");
    checkEqInt(layerpanel::layerIndexToRow(4, 5), 0, "索引 4 在第 0 行");
    checkEqInt(layerpanel::layerIndexToRow(0, 5), 4, "索引 0 在第 4 行");

    // ★ 互逆性：对每个图层数、每个位置都验一遍 ★
    //   只验 5 这一种规模的话，"count 的奇偶"或"count == 1"这类边界
    //   漏掉了也不会知道。
    bool inverseOk = true;
    for (int count = 1; count <= 9; ++count) {
        for (int i = 0; i < count; ++i) {
            const int row = layerpanel::layerIndexToRow(i, count);
            if (layerpanel::rowToLayerIndex(row, count) != i)
                inverseOk = false;
        }
    }
    check(inverseOk, "两个方向互为逆映射（图层数 1..9 全覆盖）");

    // 越界
    checkEqInt(layerpanel::rowToLayerIndex(5, 5), -1, "row 越上界 → -1");
    checkEqInt(layerpanel::rowToLayerIndex(-1, 5), -1, "row 为负 → -1");
    checkEqInt(layerpanel::rowToLayerIndex(0, 0), -1, "空文档 → -1");
    checkEqInt(layerpanel::layerIndexToRow(5, 5), -1, "index 越上界 → -1");
    checkEqInt(layerpanel::layerIndexToRow(0, 0), -1, "空文档 → -1");

    // 单图层：唯一合法的映射是两个 0
    checkEqInt(layerpanel::rowToLayerIndex(0, 1), 0, "单图层：行 0 = 索引 0");
    checkEqInt(layerpanel::layerIndexToRow(0, 1), 0, "单图层：索引 0 = 行 0");

    // 批量：保持输入顺序 + 丢弃越界项
    const QList<int> rows = { 0, 2, 4 };
    const QList<int> got = layerpanel::rowsToLayerIndices(rows, 5);
    checkEqInt(got.size(), 3, "批量换算：三个都保留");
    check(got == QList<int>({ 4, 2, 0 }),
          "批量换算：按输入行序给出索引（4,2,0）——顺序必须保持，因为调用方"
          "要靠它把「选中的行」一一对回图层");

    const QList<int> dirty = { -1, 0, 99, 2 };
    const QList<int> got2 = layerpanel::rowsToLayerIndices(dirty, 5);
    check(got2 == QList<int>({ 4, 2 }),
          "批量换算：丢弃越界项（-1 与 99），其余按原序保留");
    check(layerpanel::rowsToLayerIndices(QList<int>(), 5).isEmpty(),
          "批量换算：空输入 → 空输出");
}

// ===========================================================================
//  [2] 行内热点
// ===========================================================================
static void testHotspots()
{
    beginGroup("行内热点（眼睛 / 锁 / 名字区）");

    const QRect row(0, 0, 240, layerpanel::kRowHeight);

    const QRect thumb = layerpanel::thumbnailRect(row);
    const QRect eye   = layerpanel::eyeRect(row);
    const QRect lock  = layerpanel::lockRect(row);
    const QRect name  = layerpanel::nameRect(row);

    check(row.contains(thumb), "缩略图矩形落在行内");
    check(row.contains(eye),   "眼睛矩形落在行内");
    check(row.contains(lock),  "锁矩形落在行内");

    // ★ 三个矩形两两不重叠 ★
    //   重叠本身不算错，但一旦重叠，命中就取决于判定顺序 ——
    //   那正是"点眼睛变成了选图层"这类 bug 的温床。这里要求它们本来就分开。
    check(!eye.intersects(lock),   "眼睛与锁的矩形不重叠");
    check(!eye.intersects(name),   "眼睛与名字区不重叠");
    check(!lock.intersects(name),  "锁与名字区不重叠");
    check(thumb.right() < name.left(), "缩略图在名字区左侧");

    // 命中判定
    check(layerpanel::hotspotAt(row, thumb.center()) == layerpanel::Hotspot::Thumbnail,
          "缩略图中心 → Thumbnail（拖拽排序 / 双击重命名 / 切活动图层都走它）");
    check(layerpanel::hotspotAt(row, eye.center()) == layerpanel::Hotspot::Visible,
          "眼睛中心 → Visible");
    check(layerpanel::hotspotAt(row, lock.center()) == layerpanel::Hotspot::Lock,
          "锁中心 → Lock");
    check(layerpanel::hotspotAt(row, name.center()) == layerpanel::Hotspot::Thumbnail,
          "名字区中心 → Thumbnail（它是兜底分支）");

    // 行外
    check(layerpanel::hotspotAt(row, QPoint(row.center().x(), row.bottom() + 3))
              == layerpanel::Hotspot::Outside,
          "行的下方 3 像素 → Outside");
    check(layerpanel::hotspotAt(row, QPoint(row.center().x(), row.top() - 1))
              == layerpanel::Hotspot::Outside,
          "行的上方 1 像素 → Outside");
    check(layerpanel::hotspotAt(row, QPoint(row.right() + 1, row.center().y()))
              == layerpanel::Hotspot::Outside,
          "行的右侧 1 像素 → Outside");

    // ★ 边界像素：矩形的两个角必须判在自己这一边 ★
    //   QRect::contains 是闭区间。若哪天有人把它换成半开区间，
    //   右上角那个像素就会漏判 —— 而它恰好是用户容易点到的位置。
    check(layerpanel::hotspotAt(row, eye.topLeft()) == layerpanel::Hotspot::Visible,
          "眼睛矩形的左上角算命中");
    check(layerpanel::hotspotAt(row, eye.bottomRight()) == layerpanel::Hotspot::Visible,
          "眼睛矩形的右下角算命中");
}

// ===========================================================================
//  [3] 拖拽落点 ⇄ 真实 moveLayer 对拍
// ===========================================================================
static void testDropTargets()
{
    beginGroup("拖拽落点 → 目标索引（与真实 moveLayer 对拍）");

    const QStringList names = { QStringLiteral("底"), QStringLiteral("中"),
                                QStringLiteral("上"), QStringLiteral("顶") };
    const int count = names.size();

    QStringList refSorted = names;
    refSorted.sort();

    int checkedCombos = 0;
    bool allPermutation = true;
    bool allLandedRight = true;
    bool allMoved = true;

    for (int src = 0; src < count; ++src) {
        // dropBeforeRow 取遍 0..count（含两条边界：插到最顶 / 插到最底）
        for (int dropBeforeRow = 0; dropBeforeRow <= count; ++dropBeforeRow) {
            const int to = layerpanel::dropRowToTargetIndex(src, dropBeforeRow, count);
            if (to < 0 || to >= count) {
                std::printf("  [失败] src=%d drop=%d 给出的目标索引越界：%d\n",
                            src, dropBeforeRow, to);
                ++g_failed;
                continue;
            }

            Document *doc = makeDoc(names);
            const bool moved = doc->moveLayer(src, to);
            const QStringList after = visualNames(doc);
            const QStringList expected = modelVisualNames(names, src, dropBeforeRow);

            if (!moved)
                allMoved = false;

            QStringList afterSorted = after;
            afterSorted.sort();
            if (afterSorted != refSorted)
                allPermutation = false;

            if (after != expected)
                allLandedRight = false;

            delete doc;
            ++checkedCombos;
        }
    }

    std::printf("       对拍组合 %d 组（4 图层 × 5 个落点）\n", checkedCombos);
    checkEqInt(checkedCombos, count * (count + 1), "覆盖了全部 (源, 落点) 组合");
    check(allMoved, "每一组 moveLayer 都返回成功");
    check(allPermutation, "拖完之后仍是一个完整排列（没丢图层、没多图层）");
    check(allLandedRight,
          "每一组拖完之后，被拖动的那一项都落在语义模型算出的视图行上");

    // 原地放回：目标索引必须等于源索引
    //   （面板据此**不**发 reorderRequested，避免往撤销栈里塞空操作）
    for (int i = 0; i < count; ++i) {
        const int row = layerpanel::layerIndexToRow(i, count);
        const int to = layerpanel::dropRowToTargetIndex(i, row, count);
        checkEqInt(to, i, "拖回自己所在的行 → 目标索引不变");
    }

    // 越界输入
    checkEqInt(layerpanel::dropRowToTargetIndex(0, 0, 0), -1, "空文档 → -1");
    checkEqInt(layerpanel::dropRowToTargetIndex(-1, 0, 4), -1, "源索引为负 → -1");
    checkEqInt(layerpanel::dropRowToTargetIndex(4, 2, 4), -1, "源索引越界 → -1");
    // 落点越界要被钳制，而不是返回 -1：拖拽时鼠标可以拖到列表外面。
    check(layerpanel::dropRowToTargetIndex(0, 99, 4) >= 0,
          "落点远超范围 → 钳制到最底，不返回 -1");
    check(layerpanel::dropRowToTargetIndex(0, -5, 4) >= 0,
          "落点为负 → 钳制到最顶，不返回 -1");
}

// ===========================================================================
//  [4] 面板 ⇄ 文档同步
// ===========================================================================
static void testPanelSync()
{
    beginGroup("面板 ⇄ 文档同步");

    const QStringList names = { QStringLiteral("底层"), QStringLiteral("中层"),
                                QStringLiteral("顶层") };
    Document *doc = makeDoc(names);

    LayerPanel panel(60000);          // 节流窗口开很大，避免干扰本组断言
    panel.resize(260, 420);
    panel.show();
    QApplication::processEvents();
    panel.setDocument(doc);

    QListWidget *list = panel.listWidget();
    check(list != nullptr, "面板拿得到列表控件");
    if (!list || list->count() != doc->layerCount()) {
        check(false, "面板行数与图层数一致");
        delete doc;
        return;
    }

    // ── 行数与行序 ──
    checkEqInt(list->count(), 3, "行数恒等于图层数");
    checkEqStr(list->item(0)->text(), QStringLiteral("顶层"),
               "第 0 行显示的是**最上面**那个图层");
    checkEqStr(list->item(2)->text(), QStringLiteral("底层"),
               "最后一行显示的是**最下面**那个图层");

    // Qt::UserRole 是"与旧版最小列表保持兼容"的公开契约：行 → 文档索引
    checkEqInt(list->item(0)->data(Qt::UserRole).toInt(), 2,
               "第 0 行的 UserRole = 索引 2（最顶）");
    checkEqInt(list->item(2)->data(Qt::UserRole).toInt(), 0,
               "最后一行的 UserRole = 索引 0（最底）");

    bool rolesOk = true;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toInt()
                != layerpanel::rowToLayerIndex(row, list->count()))
            rolesOk = false;
    }
    check(rolesOk, "每一行的 UserRole 都等于 rowToLayerIndex(row)");

    // ── 活动图层那一行是"当前行" ──
    doc->setActiveLayerIndex(1);
    QApplication::processEvents();
    check(list->currentItem() == list->item(1),
          "活动图层换成索引 1 后，第 1 行成为当前行（行序倒过来也要跟对）");

    // ── 结构变化 → 行跟着变 ──
    //   活动图层现在是 1，所以新图层插在索引 2（= 视图第 1 行）。
    doc->addLayer(QStringLiteral("新加的"));
    QApplication::processEvents();
    checkEqInt(list->count(), 4, "加一个图层后行数变 4");
    checkEqStr(list->item(1)->text(), QStringLiteral("新加的"),
               "新图层插在活动图层之上 → 落在视图第 1 行（不是第 0 行）");
    checkEqStr(list->item(0)->text(), QStringLiteral("顶层"),
               "原来在最顶的图层仍在视图第 0 行");

    // ── 属性变化 → 行文字跟着变 ──
    doc->layers().mutableAll()[0].setName(QStringLiteral("改过名的"));
    doc->markDirty();
    pokeStructure(doc);
    checkEqStr(list->item(3)->text(), QStringLiteral("改过名的"),
               "最底层图层改名后，最后一行跟着变");

    // ── 换文档 ──
    Document *other = makeDoc(QStringList({ QStringLiteral("只有一层") }));
    panel.setDocument(other);
    QApplication::processEvents();
    checkEqInt(list->count(), 1, "换文档后行数变成新文档的图层数");
    checkEqStr(list->item(0)->text(), QStringLiteral("只有一层"),
               "换文档后行内容是新文档的");

    // ★ 旧文档不再干扰面板 ★
    //   少了"先断开旧文档"这一步，改旧文档会把面板刷成旧文档的内容 ——
    //   多标签化之后，这就是最典型的"面板显示的不是当前文档"。
    doc->addLayer(QStringLiteral("旧文档加的一层"));
    doc->markDirty();
    pokeStructure(doc);
    checkEqInt(list->count(), 1, "改旧文档不会把面板刷成旧文档的内容");

    // ── 拖拽能力确实是"装上"的 ──
    check(list->dragDropMode() == QAbstractItemView::InternalMove,
          "列表的拖放模式是 InternalMove");
    check(list->item(0)->flags().testFlag(Qt::ItemIsDragEnabled),
          "行可以被拖动（ItemIsDragEnabled）");
    check(list->item(0)->flags().testFlag(Qt::ItemIsEditable),
          "行可以被重命名（ItemIsEditable）");
    check(list->selectionMode() == QAbstractItemView::ExtendedSelection,
          "支持多选（ExtendedSelection）");

    delete other;
    delete doc;
}

// ===========================================================================
//  [5] 面板只发信号，不改文档
// ===========================================================================
static void testPanelEmitsOnly()
{
    beginGroup("面板只发信号，自己绝不改文档");

    const QStringList names = { QStringLiteral("底"), QStringLiteral("中"),
                                QStringLiteral("顶") };
    Document *doc = makeDoc(names);
    // 活动图层故意设成"不在第 0 行"的那个，用来验证"点眼睛不切活动图层"
    doc->setActiveLayerIndex(0);

    LayerPanel panel(60000);
    panel.resize(260, 420);
    panel.show();
    QApplication::processEvents();
    panel.setDocument(doc);

    QListWidget *list = panel.listWidget();
    if (!list || list->count() < 3) {
        check(false, "面板还没准备好（行数不足）");
        delete doc;
        return;
    }

    // ── 点第 0 行（最顶 = 索引 2）的眼睛 ──
    int sigIndex = -1;
    bool sigVisible = false;
    int sigCount = 0;
    QObject::connect(&panel, &LayerPanel::visibilityToggled,
                     [&](int index, bool visible) {
        ++sigCount;
        sigIndex = index;
        sigVisible = visible;
    });

    const bool visibleBefore = doc->layers().layer(2)->isVisible();
    const int activeBefore = doc->activeLayerIndex();

    clickAt(list, layerpanel::eyeRect(rowRectFor(list, 0)).center());
    QApplication::processEvents();

    checkEqInt(sigCount, 1, "点眼睛发出一次 visibilityToggled");
    checkEqInt(sigIndex, 2, "信号带的是**文档索引** 2（不是视图行 0）");
    check(sigVisible == !visibleBefore, "信号带的是取反后的可见性");
    check(doc->layers().layer(2)->isVisible() == visibleBefore,
          "★ 文档没有被改动 —— 面板只发信号，改动留给 MainWindow 包成命令");
    checkEqInt(doc->activeLayerIndex(), activeBefore,
               "点眼睛不改变活动图层（与 Photoshop 一致：可以连续关掉几个）");

    // ── 点最后一行（最底 = 索引 0）的锁 ──
    int lockIndex = -1;
    bool lockValue = false;
    int lockCount = 0;
    QObject::connect(&panel, &LayerPanel::lockToggled,
                     [&](int index, bool locked) {
        ++lockCount;
        lockIndex = index;
        lockValue = locked;
    });

    const bool lockedBefore = doc->layers().layer(0)->isLocked();
    clickAt(list, layerpanel::lockRect(rowRectFor(list, 2)).center());
    QApplication::processEvents();

    checkEqInt(lockCount, 1, "点锁发出一次 lockToggled");
    checkEqInt(lockIndex, 0, "锁信号带的是文档索引 0（最底那层）");
    check(lockValue == !lockedBefore, "锁信号带的是取反后的状态");
    check(doc->layers().layer(0)->isLocked() == lockedBefore,
          "★ 文档的锁定状态没有被改动");

    // ── 点缩略图/名字区：不发眼睛/锁信号，而是切换活动图层 ──
    //   "切活动图层"是面板唯一允许自己写文档的地方（纯 UI 状态、不进撤销栈）。
    //   这里把它一并钉住，免得将来有人顺手把文件内容也在这里改掉。
    const int visCountBefore = sigCount;
    const int nameRowTop = list->visualRect(list->model()->index(1, 0)).top();
    const QPoint namePos = layerpanel::nameRect(
                QRect(0, nameRowTop, list->viewport()->width(),
                      layerpanel::kRowHeight)).center();
    clickAt(list, namePos);
    QApplication::processEvents();

    checkEqInt(sigCount, visCountBefore, "点名字区不会再发一次可见性信号");
    checkEqInt(doc->activeLayerIndex(), 1,
               "点第 1 行（索引 1）的名字区 → 活动图层变成 1");

    // ── 重命名：delegate 提交 → 面板转发，文档仍不动 ──
    int renameIndex = -1;
    QString renameName;
    int renameCount = 0;
    QObject::connect(&panel, &LayerPanel::renameRequested,
                     [&](int index, const QString &name) {
        ++renameCount;
        renameIndex = index;
        renameName = name;
    });

    if (auto *delegate = qobject_cast<LayerItemDelegate *>(list->itemDelegate())) {
        emit delegate->commitRename(0, QStringLiteral("新名字"));
        QApplication::processEvents();

        checkEqInt(renameCount, 1, "delegate 提交重命名 → 面板转发一次");
        checkEqInt(renameIndex, 2,
                   "重命名信号带的是文档索引 2（视图第 0 行 → 最顶那层）");
        checkEqStr(renameName, QStringLiteral("新名字"), "名字原样转发");
        checkEqStr(doc->layers().layer(2)->name(), QStringLiteral("顶"),
                   "★ 文档里的图层名没有被改 —— 面板不写 model，也不写文档");
    } else {
        check(false, "列表用的是 LayerItemDelegate");
    }

    delete doc;
}

// ===========================================================================
//  [6] 自绘行的像素（"量它画出来的东西"）
// ===========================================================================
static void testPaintedPixels()
{
    beginGroup("自绘行的像素（铁律：每条自绘管线都要量一次）");

    Document *doc = makeDoc(QStringList({ QStringLiteral("红图层") }));

    LayerPanel panel(60000);
    panel.resize(260, 420);
    panel.show();
    QApplication::processEvents();
    panel.setDocument(doc);

    QListWidget *list = panel.listWidget();
    if (!list || list->count() < 1) {
        check(false, "面板还没准备好");
        delete doc;
        return;
    }

    // ★ 给图层铺满纯红 ★
    //   这样"缩略图区红不红"就成了一个可量化的判据：
    //   · ThumbnailRole 没写 → 只有棋盘格底，一点都不红
    //   · 缩略图画到了别的行/别的位置 → 这里量不到红
    //   数学全绿而画面不对时，这类断言是唯一能说话的东西。
    doc->layers().mutableAll()[0].setImage(
                solidImage(64, 48, QColor(255, 0, 0)));
    doc->markDirty();
    pokeStructure(doc);

    const QRect rowRect = rowRectFor(list, 0);
    const QRect thumbRect = layerpanel::thumbnailRect(rowRect);

    // 往里缩 3 像素：避开 1 像素描边，以及"保持比例"留下的透明边
    const QImage thumbShot = grabRegion(list, thumbRect.adjusted(3, 3, -3, -3));
    check(!thumbShot.isNull() && thumbShot.width() > 5,
          "抓得到缩略图区域的像素");
    if (!thumbShot.isNull() && thumbShot.width() > 5) {
        const int total = thumbShot.width() * thumbShot.height();
        const double ratio = double(countReddish(thumbShot)) / double(total);
        std::printf("       缩略图区红色像素占比 %.1f%%\n", ratio * 100.0);
        check(ratio > 0.5, "★ 缩略图区大部分是红色 —— 图层位图真的画上去了");
    }

    // ★ 眼睛图标真的画出来了 ★
    const QRect eyeRect = layerpanel::eyeRect(rowRect);
    const QImage eyeVisible = grabRegion(list, eyeRect);
    check(countDark(eyeVisible) > 10,
          "可见状态的眼睛区有深色描边（图标不是空白）");

    // 切到"隐藏"：眼睛图标换成另一种（带斜杠），且透明度从 0.92 降到 0.45
    doc->layers().mutableAll()[0].setVisible(false);
    doc->markDirty();
    pokeStructure(doc);

    const QImage eyeHidden = grabRegion(list, layerpanel::eyeRect(rowRectFor(list, 0)));
    const int eyeDiff = diffPixels(eyeVisible, eyeHidden);
    std::printf("       眼睛区可见/隐藏两种状态相差 %d 个像素\n", eyeDiff);
    check(eyeDiff > 20,
          "★ 可见 / 隐藏两种状态的眼睛区画得不一样（不是同一个图标）");

    // ★ 锁也一样 ★
    const QImage lockUnlocked = grabRegion(list, layerpanel::lockRect(rowRectFor(list, 0)));
    doc->layers().mutableAll()[0].setLocked(true);
    doc->markDirty();
    pokeStructure(doc);
    const QImage lockLocked = grabRegion(list, layerpanel::lockRect(rowRectFor(list, 0)));
    const int lockDiff = diffPixels(lockUnlocked, lockLocked);
    std::printf("       锁区未锁/已锁两种状态相差 %d 个像素\n", lockDiff);
    check(lockDiff > 20,
          "★ 未锁 / 已锁两种状态的锁区画得不一样");

    // ★ 隐藏时整行变淡：名字区的像素必须跟着变 ★
    //   （旧版最小列表的视觉约定，用户已经习惯"灰掉的就是隐藏的"。）
    doc->layers().mutableAll()[0].setVisible(true);
    doc->markDirty();
    pokeStructure(doc);
    const QImage nameVisible = grabRegion(list, layerpanel::nameRect(rowRectFor(list, 0)));
    doc->layers().mutableAll()[0].setVisible(false);
    doc->markDirty();
    pokeStructure(doc);
    const QImage nameHidden = grabRegion(list, layerpanel::nameRect(rowRectFor(list, 0)));
    check(diffPixels(nameVisible, nameHidden) > 5,
          "隐藏后名字区变淡（文字像素确实变了）");

    delete doc;
}

// ===========================================================================
//  [7] 属性控件
// ===========================================================================
static void testPropertyControls()
{
    beginGroup("混合模式 / 不透明度控件");

    const QStringList names = { QStringLiteral("A"), QStringLiteral("B") };
    Document *doc = makeDoc(names);
    doc->setActiveLayerIndex(1);
    doc->layers().mutableAll()[1].setOpacity(255);

    LayerPanel panel(60000);
    panel.resize(260, 420);
    panel.show();
    QApplication::processEvents();
    panel.setDocument(doc);

    QComboBox *combo = panel.blendCombo();
    QSlider   *slider = panel.opacitySlider();
    check(combo != nullptr && slider != nullptr, "面板上有混合模式与不透明度控件");
    if (!combo || !slider) {
        delete doc;
        return;
    }

    // ── 回填：构造后控件必须反映活动图层的现状 ──
    checkEqInt(combo->currentData().toInt(), int(BlendMode::Normal),
               "下拉框回填成活动图层的混合模式（正常）");
    checkEqInt(slider->value(), 100, "滑块回填成活动图层的不透明度（100%）");

    // ★ 回填不能反过来改文档 ★
    //   这是"改控件 → 触发信号 → 又改文档"那条环路的守门断言。
    //   面板用 m_syncing 抑制住它；一旦哪天有人去掉那个抑制，
    //   这一条会立刻红，而不是等到用户发现"切图层把图层属性改了"。
    checkEqInt(int(doc->layers().layer(1)->blendMode()), int(BlendMode::Normal),
               "★ 回填控件没有反过来改文档（混合模式）");
    checkEqInt(int(doc->layers().layer(1)->opacity()), 255,
               "★ 回填控件没有反过来改文档（不透明度）");

    // ── 换活动图层 → 控件跟着走 ──
    doc->layers().mutableAll()[0].setOpacity(128);
    doc->layers().mutableAll()[0].setBlendMode(BlendMode::Screen);
    doc->setActiveLayerIndex(0);
    QApplication::processEvents();
    checkEqInt(combo->currentData().toInt(), int(BlendMode::Screen),
               "换到索引 0 后下拉框显示「滤色」");
    checkEqInt(slider->value(), 50, "换到索引 0 后滑块显示 50%");
    doc->setActiveLayerIndex(1);
    QApplication::processEvents();
    checkEqInt(combo->currentData().toInt(), int(BlendMode::Normal),
               "换回索引 1 后下拉框回到「正常」");
    checkEqInt(slider->value(), 100, "换回索引 1 后滑块回到 100%");

    // ── 混合模式：一次改动 = 一次信号 ──
    int blendIndex = -1, blendBefore = -1, blendAfter = -1, blendCount = 0;
    QObject::connect(&panel, &LayerPanel::blendModeChanged,
                     [&](int index, int before, int after) {
        ++blendCount;
        blendIndex = index;
        blendBefore = before;
        blendAfter = after;
    });

    const int mulIndex = combo->findData(int(BlendMode::Multiply));
    check(mulIndex >= 0, "下拉框里有「正片叠底」这一项");
    if (mulIndex >= 0) {
        combo->setCurrentIndex(mulIndex);
        QApplication::processEvents();

        checkEqInt(blendCount, 1, "换一次混合模式发一次信号");
        checkEqInt(blendIndex, 1, "信号带的是活动图层索引 1");
        checkEqInt(blendBefore, int(BlendMode::Normal), "before = 正常");
        checkEqInt(blendAfter, int(BlendMode::Multiply), "after = 正片叠底");
        checkEqInt(int(doc->layers().layer(1)->blendMode()), int(BlendMode::Normal),
                   "★ 文档没有被改 —— 面板只发信号");
    }

    // ── 不透明度：非拖动（键盘 / 点轨道）→ 一步一条命令 ──
    int opIndex = -1, opBefore = -1, opAfter = -1, commitCount = 0, previewCount = 0;
    QObject::connect(&panel, &LayerPanel::opacityCommitted,
                     [&](int index, int before, int after) {
        ++commitCount;
        opIndex = index;
        opBefore = before;
        opAfter = after;
    });
    QObject::connect(&panel, &LayerPanel::opacityPreviewed,
                     [&](int, int) { ++previewCount; });

    slider->setValue(40);
    QApplication::processEvents();

    checkEqInt(previewCount, 0,
               "滑块未被按下时走提交这条路，不发预览信号");
    checkEqInt(commitCount, 1, "改一次值发一次 opacityCommitted");
    checkEqInt(opIndex, 1, "不透明度信号带的是活动图层索引 1");
    checkEqInt(opBefore, 255, "before = 改动前的 255");
    checkEqInt(opAfter, qRound(40 * 255.0 / 100.0), "after = 40% 换算成 0~255");
    checkEqInt(int(doc->layers().layer(1)->opacity()), 255,
               "★ 文档的不透明度没有被改 —— 面板只发信号");

    // ── 不透明度：拖动中 → 只发预览，不提交 ──
    //   ★ 这一条守的是"撤销栈不被污染" ★
    //     拖动一次滑块会发几十次 valueChanged。若它们都变成"提交"，
    //     用户要按几十次 Ctrl+Z 才能退回去。
    const int commitBeforeDrag = commitCount;
    const int previewBeforeDrag = previewCount;

    slider->setSliderDown(true);
    for (int pct = 41; pct <= 60; ++pct)
        slider->setValue(pct);
    QApplication::processEvents();

    checkEqInt(previewCount - previewBeforeDrag, 20,
               "拖动中 20 次变化 → 20 次 opacityPreviewed");
    checkEqInt(commitCount, commitBeforeDrag,
               "★ 拖动中一次都不提交 —— 松手才压一条命令");
    checkEqInt(int(doc->layers().layer(1)->opacity()), 255,
               "拖动中面板依然不碰文档（实时预览由 MainWindow 负责）");
    slider->setSliderDown(false);

    delete doc;
}

// ===========================================================================
//  [8] 缩略图节流
// ===========================================================================
static void testThumbnailThrottle()
{
    beginGroup("缩略图节流");

    const QStringList names = { QStringLiteral("A"), QStringLiteral("B"),
                                QStringLiteral("C") };
    Document *doc = makeDoc(names);

    // 节流窗口开得很大：本组要证明的是"变化不会立刻导致重算"，
    // 而不是"多久之后会重算" —— 后者要靠等真实时间，会做成 flaky 测试。
    LayerPanel panel(60000);
    panel.resize(260, 420);
    panel.show();
    QApplication::processEvents();
    panel.setDocument(doc);

    const int afterSetup = panel.thumbnailRenderCount();
    check(afterSetup >= 3, "初次绑定文档时生成了每一行的缩略图");
    checkEqInt(panel.throttleIntervalMs(), 60000,
               "节流窗口就是构造时传进去的那个值");
    check(!panel.throttlePending(), "刚绑定时没有待处理的缩略图刷新");

    // 连打 20 次 contentChanged（模拟画笔的 20 笔）
    for (int i = 0; i < 20; ++i) {
        doc->markDirty();
        QApplication::processEvents();
    }

    checkEqInt(panel.thumbnailRenderCount(), afterSetup,
               "★ 20 次内容变化没有触发任何一次缩略图重算（节流生效）");
    check(panel.throttlePending(), "但确实排了一次待处理的刷新");

    // 手动立刻刷一次（等价于计时器到点）
    panel.refreshThumbnailsNow();
    QApplication::processEvents();
    checkEqInt(panel.thumbnailRenderCount(), afterSetup + doc->layerCount(),
               "刷新一次 = 每个图层重算一张缩略图，且只有一批");

    // 再打 20 次，计数器必须**只**因为"再刷新一次"而变化
    const int afterFirstFlush = panel.thumbnailRenderCount();
    for (int i = 0; i < 20; ++i) {
        doc->markDirty();
        QApplication::processEvents();
    }
    checkEqInt(panel.thumbnailRenderCount(), afterFirstFlush,
               "第二批 20 次变化同样一次都不重算");

    // ★ 节流不是防抖 ★
    //   防抖会把计时器每次都 restart()，"手不停就一直不刷新"。
    //   判据：计时器已经在跑的时候再排一次，不能把它取消或推后。
    //   本组窗口是 60s，测试不可能跑那么久，所以"仍在跑"就等价于"没被推后"。
    panel.refreshThumbnailsNow();
    doc->markDirty();
    QApplication::processEvents();
    check(panel.throttlePending(),
          "★ 重复排期不会取消已排好的那次刷新（是节流，不是防抖）");

    // 换文档之后不再收到旧文档的节流排期
    Document *other = makeDoc(QStringList({ QStringLiteral("X") }));
    panel.setDocument(other);
    QApplication::processEvents();

    // ★ 先把计时器停掉，让"是否排期"从一个干净状态开始量 ★
    //   上面那条"是节流不是防抖"刻意留了一次**待处理**的刷新。
    //   不清掉它的话，下面读到的会是上一次的残留，断言就成了假的
    //   （实测踩过：同一句话既可能因为"残留"假红，也可能因为
    //     "反正一直是 true"而假绿）。
    panel.setThrottleIntervalMs(60000);
    check(!panel.throttlePending(), "（前置）计时器已清空，后面量的是新排期");

    const int afterSwitch = panel.thumbnailRenderCount();
    doc->markDirty();
    QApplication::processEvents();
    checkEqInt(panel.thumbnailRenderCount(), afterSwitch,
               "旧文档的内容变化不会再触发本面板的缩略图重算");
    check(!panel.throttlePending(),
          "★ 换文档后旧文档再也排不动本面板的刷新（断连真的做了）");

    // 新文档自己的变化仍然有效（否则"断连"就把新连接一起断掉了）
    other->markDirty();
    QApplication::processEvents();
    check(panel.throttlePending(), "新文档的变化仍然能排起刷新");

    delete other;
    delete doc;
}

// ===========================================================================

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");

    // ★ offscreen 平台默认**没有字体**（Qt 5 起不再随包分发字体），
    //   没有字体时所有文字都画成空白 —— 于是这一组里那条
    //   "隐藏后名字区变淡"会量到两张一模一样的空图。
    //   实测踩过：第一次跑就报了这一条失败，而画面其实是对的。
    //   （同目录的 icon_test 不受影响，它不量文字。）
    //   只在目录真的存在时才设，免得在别的机器上把 Qt 指向不存在的路径。
    if (QDir(QStringLiteral("C:/Windows/Fonts")).exists())
        qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");

    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 图层面板 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testRowMapping();
    testHotspots();
    testDropTargets();
    testPanelSync();
    testPanelEmitsOnly();
    testPaintedPixels();
    testPropertyControls();
    testThumbnailThrottle();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
