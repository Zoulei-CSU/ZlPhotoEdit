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
    //   缓存的失效粒度是"整个画布"。更精细的做法是脏矩形，
    //   但标准版的画布尺寸通常在几百万像素量级，一次合成为 10~30ms，
    //   配合下面的降采样策略已经够用。
    QImage composite();

    // 强制缓存失效。任何修改图层/像素的操作后都必须调用。
    void markDirty();

    // 修订号。每次 markDirty() 递增。
    // CanvasView 可以通过比较修订号判断"需不需要重绘"，
    // 避免无意义的 paintEvent。
    quint64 revision() const { return m_revision; }

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

    // 内容包围盒缓存
    mutable QRect m_contentBoundsCache;
    mutable bool  m_contentBoundsValid = false;

    QString m_filePath;
    bool    m_modified = false;

    qreal m_dpiX = 72.0;
    qreal m_dpiY = 72.0;
};

} // namespace zl
