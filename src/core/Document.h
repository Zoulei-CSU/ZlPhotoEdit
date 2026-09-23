#pragma once

// ============================================================================
//  Document.h —— 文档（像素数据的唯一来源）
// ----------------------------------------------------------------------------
//  架构铁律（违反会导致整个架构失效）：
//    ★ Document 是全部像素数据的**唯一持有者**。
//      CanvasView 只做呈现，不存副本（可以有缓存，但必须能从 Document 重建）。
//      工具不直接写像素，只产生 QUndoCommand。
//
//  这个约束带来的好处：
//    · 撤销重做只需要在 Document 层面处理，视图自动跟随
//    · 多视图（缩略图、导航器、图层缩略图）自动保持一致
//    · 不需要维护"数据同步"这类逻辑，因为只有一个数据源
//
//  几何尺寸的组织方式：
//    Document 有一个**画布尺寸**（m_canvasSize），所有图层位图的尺寸
//    都等于画布尺寸。图像大小 / 画布大小 / 旋转 / 裁剪这些操作会同时
//    改变画布尺寸与所有图层位图。
//
//    为什么不采用"图层尺寸各自独立 + 各自偏移"的设计（即 Photoshop 的
//    智能对象式图层）？因为那需要每个图层维护一个位置偏移，合成时要
//    做坐标对齐，复杂度高很多。标准版的图层都是"贴满画布"的普通图层，
//    统一尺寸的模型简单可靠，且完全够用。
// ============================================================================

#include "core/ImageTypes.h"
#include "core/Layer.h"
#include "core/LayerList.h"
#include "core/Selection.h"
#include "engine/Geometry.h"

#include <QObject>
#include <QImage>
#include <QString>
#include <QSize>
#include <QRect>

namespace zl {

class Document : public QObject
{
    Q_OBJECT

public:
    // ── 构造 / 析构 ──

    // 创建一个空文档。size 为画布尺寸，fillColor 为初始背景
    // （透明 / 白色 / 背景色，由"新建"对话框选择）。
    explicit Document(const QSize &size, const QColor &fillColor = Qt::transparent,
                      QObject *parent = nullptr);

    ~Document() override;

    // ── 标识 ──

    // 文档的唯一标识。
    // 用途：剪贴板判断"是不是从本文档复制的"，从而决定能否原位粘贴。
    // 用指针地址而不是自增 ID —— 文档销毁后地址可能被复用，
    // 但剪贴板数据的生命周期通常短于文档，实际不会出问题；
    // 而且指针比较是 O(1)，不需要维护全局计数器。
    quintptr id() const { return reinterpret_cast<quintptr>(this); }

    // ── 画布 ──

    QSize canvasSize() const { return m_canvasSize; }
    QRect canvasRect() const { return QRect(QPoint(0, 0), m_canvasSize); }

    // ── 图层 ──

    LayerList       &layers() { return m_layers; }
    const LayerList &layers() const { return m_layers; }

    int  layerCount() const { return m_layers.count(); }
    int  activeLayerIndex() const { return m_activeLayerIndex; }

    // 切换活动图层。索引越界时忽略（而不是设为 -1），
    // 因为 Document 必须始终有一个活动图层，否则画笔不知道往哪画。
    void setActiveLayerIndex(int index);

    Layer       *activeLayer();
    const Layer *activeLayer() const;

    // ── 选区 ──

    Selection       &selection() { return m_selection; }
    const Selection &selection() const { return m_selection; }

    // 选区的尺寸必须与画布一致。所有会改变画布尺寸的操作都要调用它。
    void syncSelectionToCanvas(const QPoint &offset = QPoint());

    // ── 合成（呈现的核心）──

    // 把所有可见图层按混合模式合成，返回结果。
    //
    // ★ 性能约定 ★
    //   本函数被 paintEvent 高频调用。为了不每次重算，内部维护一个缓存：
    //   只要图层内容没变（由一个"修订号"标识），就直接返回缓存。
    //   任何修改像素或图层属性的操作都必须调用 markDirty()。
    //
    // ★ 失效粒度：局部（脏矩形）为主、全量为兜底 ★
    //
    //   这里原先的注释写着"缓存的失效粒度是整个画布 …… 一次合成为
    //   10~30ms，已经够用"。**这个前提已被实测推翻**（2026-09-21）：
    //   实际一次全画布合成是 146 ~ 4134 ms，比该估计慢 5 ~ 140 倍 ——
    //   因为今天的常见输入是 24MP 相机照片（6000×4000），而不是
    //   该注释假设的 2000×1500 量级。于是"每个鼠标移动重算整张画布"
    //   直接把画笔变成了幻灯片（4K / 3 图层 = 2.3 fps）。
    //
    //   现在的模型：
    //     · markDirtyRegion(rect) —— 快路径，调用方明确知道只改了哪一块
    //     · markDirty()           —— 兜底，语义仍是"整图失效"，永久保留
    //   实测收益：4K / 3 图层 431 ms → 0.25 ms。详见 docs/性能优化评估.md
    QImage composite();

    // 强制缓存失效（= 整画布都脏）。任何修改图层/像素的操作后都必须调用。
    // ★ 这个无参版本**永久保留**，它是正确性的最终保证 ★
    //   任何"我不确定这次改了什么"的地方都调它，行为与引入脏矩形之前
    //   完全一致。局部失效只是省掉重算的一条快路径，不是替代品。
    void markDirty();

    // 局部失效：只有 region 内的**像素内容**变了（图像坐标）。
    //
    // ★ 算小了比算大了危险得多 ★
    //   · 算大了 → 多算几块，只是慢一点，画面仍然正确
    //   · 算小了 → 被漏掉的区域**画面残留旧内容**：不崩、不报错、
    //     松手后可能就看不出来了，是本项目最难复现的一类 bug
    //   所以调用点必须保守，且必须有自检钉住（tests/perf_test.cpp）。
    //
    // 与 markDirty() 一样会递增修订号并发出 contentChanged()。
    void markDirtyRegion(const QRect &region);

    // 自上次成功合成以来累积的脏区域（图像坐标），供界面层决定重绘范围。
    //
    // ★ 返回空 QRect 表示"整画布都脏"（全量失效）★
    //   调用方遇到空矩形必须按全图处理 —— 不能理解成"什么都不用重绘"。
    //
    // 为什么放在这里查询、而不是把区域塞进 contentChanged 信号：
    //   改信号签名会牵动 CanvasView / LayerPanel / StatusBarWidget
    //   三处 connect，以及测试里"连打 20 次只刷一次"那类断言写法。
    //   而这里需要的信息只有画布一个人用 → 查询法改动面小得多，
    //   语义不变，其余消费者完全不受影响。
    QRect pendingDirtyRegion() const;

    // 修订号。每次 markDirty() / markDirtyRegion() 递增。
    // CanvasView 可以通过比较修订号判断"需不需要重绘"，
    // 避免无意义的 paintEvent。
    quint64 revision() const { return m_revision; }

    // ── 合成遥测（供自检与性能诊断）──
    //
    // ★ 为什么需要这两个计数 ★
    //   本功能唯一的失效方式是"表面正常、其实每次仍在全图重算"。
    //   而"composite() 返回了非空图"这类断言**抓不住**它 ——
    //   所以必须有一个能区分"走了哪条路径"的可观测量。
    //   有了计数，perf 自检才能断言"连续 20 次局部失效没有触发过
    //   任何一次全图重算"，截图工具也才能打印实测报告。
    quint64 fullCompositeCount()   const { return m_fullCompositeCount; }
    quint64 regionCompositeCount() const { return m_regionCompositeCount; }

    // ── 内容包围盒 ──

    // 所有图层非透明像素的并集包围盒（图像坐标）。
    //
    // ★ 必须缓存 ★
    //   朴素实现要遍历全部图层，4000×3000 × 4 图层 = 4800 万像素，
    //   几十毫秒。如果 paintEvent 每次都调用会严重掉帧。
    //   缓存策略：markDirty() 时置为无效，下次查询时重算。
    QRect contentBounds() const;

    // ── 几何操作 ──

    // 图像大小：缩放像素内容。所有图层与选区一起缩放。
    // 会改变画布尺寸。
    bool scaleImage(const QSize &newSize, ResampleMethod method);

    // 仅供撤销命令使用的接口：直接设置画布尺寸，**不触碰任何图层位图**。
    //
    // ★ 这是 GeometryChangeCommand 的专用入口，不要在其他地方调用 ★
    //
    // 为什么需要它：GeometryChangeCommand 的 undo/redo 是"把整个图层列表
    // 替换成一份快照"，图层位图的尺寸已经在快照里了。如果这个命令再去调
    // resizeCanvas() 之类的方法，就会对已经正确的位图再做一次变换 ——
    // 结果是撤销一次图像缩小，图画反而又小了一圈。
    //
    // 命名上带 "FromCommand" 后缀，就是为了让误用一眼可见。
    void setCanvasSizeFromCommand(const QSize &size);

    // 画布大小：像素不缩放，只改容器。
    // smart 为 true 时启用智能适配（缩小时自动平移内容避免裁切）。
    bool resizeCanvas(const QSize &newSize, Anchor anchor,
                      const QColor &fillColor, bool smart = true);

    // 图像旋转（90° 倍数，无损）。会改变画布尺寸。
    bool rotateImage90(bool clockwise);

    // 图像旋转（任意角度）。画布自动扩大到容纳旋转结果。
    //
    // fillColor 填充"旋转后四角空出来"的区域（画布按包围盒扩大，四角必然有洞）。
    // 默认透明。90° 倍数角度下走无损路径，此时 fillColor 被忽略 ——
    // 那条路径的结果与画布等大，没有空像素可填（详见 Geometry.h）。
    bool rotateImageArbitrary(qreal angleDeg, ResampleMethod method,
                              const QColor &fillColor = Qt::transparent);

    // 图像翻转。会同步翻转所有图层与选区。
    bool flipImage(FlipDirection dir);

    // 裁剪：取出指定矩形区域成为新画布。
    bool crop(const QRect &rect);

    // ── 图层操作 ──
    // 这些是"原始操作"，不做任何撤销处理 —— 撤销由 commands 层包装。
    // 之所以分成两层，是因为撤销命令需要同时记录"操作前的状态"，
    // 而这些原始操作只负责执行。

    int  addLayer(const QString &name = QString());          // 在活动图层上方新增
    int  addLayer(const Layer &layer, int index = -1);       // 插入已有图层
    bool removeLayer(int index);
    bool duplicateLayer(int index);
    bool moveLayer(int from, int to);

    // 合并两个图层（down 与 down+1），结果放在 down 位置。
    // 返回是否成功。至少要两个图层。
    bool mergeLayerDown(int index);

    // 合并所有可见图层为一个。
    bool mergeVisibleLayers();

    // 拼合图像：所有图层合并成一个，不可见图层的内容丢弃。
    bool flattenImage();

    // ── 文件状态 ──

    QString filePath() const { return m_filePath; }
    void    setFilePath(const QString &path);

    // 是否有未保存的修改。由 commands 层在压栈/撤销时通过
    // undoStack 的 cleanChanged 信号同步，这里只是个标志位。
    bool isModified() const { return m_modified; }
    void setModified(bool modified);

    // 建议的窗口标题（文件名 + 修改标记）
    QString displayName() const;

    // ── 内存估算 ──

    // 文档占用的像素内存字节数。
    // 用途：状态栏显示、大图操作前的内存预检。
    qint64 memoryUsage() const;

    // ── 元数据 ──

    qreal dpiX() const { return m_dpiX; }
    qreal dpiY() const { return m_dpiY; }
    void  setDpi(qreal x, qreal y);

signals:
    // 文档内容发生变化。CanvasView、图层面板、导航器都监听它。
    // 注意区分：
    //   contentChanged —— 像素/几何变了，需要重新合成
    //   structureChanged —— 图层增删或顺序变了，面板需要重建列表
    //   activeLayerChanged —— 只换了活动图层，面板更新选中态即可
    void contentChanged();
    void structureChanged();
    void activeLayerChanged(int index);
    void modifiedChanged(bool modified);
    void canvasSizeChanged(const QSize &size);

private:
    // 初始化：创建第一个图层
    void createInitialLayer(const QColor &fillColor);

    // 内部使用：不做任何检查地设置画布尺寸
    void setCanvasSizeInternal(const QSize &size);

    QSize     m_canvasSize;
    LayerList m_layers;
    Selection m_selection;

    int m_activeLayerIndex = 0;

    // 合成缓存
    mutable QImage  m_compositeCache;
    mutable quint64 m_revision = 0;
    mutable bool    m_compositeValid = false;

    // ── 脏区域累积（图像坐标）──
    //   m_fullInvalidate == true → 下次合成必须整图重算（m_dirtyRegion 无意义）
    //   否则                     → 只需重算 m_dirtyRegion 这一块，再贴回缓存
    //
    // ★ 为什么用两个成员，而不是"用空矩形表示全图" ★
    //   空矩形在语义上是二义的：既可能是"什么都没脏"，也可能是"整图都脏"。
    //   分开之后，"矩形为空 + 全量标志为假"这种状态根本不会出现 ——
    //   会不会重算由 m_compositeValid 单独承担。
    //   少一个二义状态，就少一类静默 bug。
    QRect m_dirtyRegion;
    bool  m_fullInvalidate = true;

    // 合成遥测计数（见头文件访问器的说明）
    mutable quint64 m_fullCompositeCount   = 0;
    mutable quint64 m_regionCompositeCount = 0;

    // 内容包围盒缓存
    mutable QRect m_contentBoundsCache;
    mutable bool  m_contentBoundsValid = false;

    QString m_filePath;
    bool    m_modified = false;

    qreal m_dpiX = 72.0;
    qreal m_dpiY = 72.0;
};

} // namespace zl
