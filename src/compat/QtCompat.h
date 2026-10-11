#pragma once

// ============================================================================
//  QtCompat.h —— Qt 5 / Qt 6 兼容层
// ----------------------------------------------------------------------------
//  设计约定（重要，请勿破坏）：
//    本工程同时支持 Qt 5.15 与 Qt 6.x。两者之间存在若干 API 差异，
//    例如 QMouseEvent::localPos() 在 Qt6 中被删除、QImage::mirrored() 改名
//    为 flipped()、QTextStream::setCodec() 被替换为 setEncoding() 等等。
//
//    如果把这些差异用 #if QT_VERSION 散落在各个 .cpp 里，将来升级 Qt 时会
//    变成一场考古工作 —— 你需要在几十个文件里找出所有版本判断。因此本工程
//    强制规定：
//
//      所有版本判断只允许出现在本文件（以及 main.cpp 的高 DPI 设置处）。
//      其余任何代码一律使用本文件提供的包装函数。
//
//    这样将来升级 Qt 时，只需要修改这一个文件。
// ============================================================================

#include <QtGlobal>
#include <QPointF>
#include <QImage>
#include <QTextStream>
#include <QString>
#include <QWheelEvent>   // wheelEventPos / wheelEventGlobalPos 需要完整类型

// Qt6 把 QStringConverter 独立出来用于文本流编码，Qt5 没有这个头。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  include <QStringConverter>
#endif

namespace zl {

// ---------------------------------------------------------------------------
//  鼠标 / 滚轮 / 触摸事件坐标
// ---------------------------------------------------------------------------
//  Qt5：QMouseEvent 提供 localPos()（返回 const QPointF&），另有弃用的 pos()
//  Qt6：localPos()、pos() 全部删除，改为 position()
//
//  用模板实现，这样 QMouseEvent、QHoverEvent、QTabletEvent 等都能共用一份
//  实现，不必为每种事件类型各写一个重载。
// ---------------------------------------------------------------------------

template <typename EventT>
inline QPointF eventPos(const EventT *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

template <typename EventT>
inline QPointF eventGlobalPos(const EventT *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition();
#else
    return e->globalPos();
#endif
}

// ---------------------------------------------------------------------------
//  滚轮事件的位置
// ---------------------------------------------------------------------------
//  ★ 为什么不能直接用上面的 eventPos() 模板 ★
//
//    QWheelEvent 是个特例：
//      · Qt 5.14 起，QWheelEvent 从 QInputEvent 改继承为 QSinglePointEvent，
//        同时提供了 position()，并把 pos() / globalPos() 标记为废弃
//      · 但 localPos() 从来就不是 QWheelEvent 的成员
//        （它属于 QMouseEvent / QHoverEvent / QTabletEvent）
//
//    所以对滚轮事件套用 eventPos() 会得到
//    "error C2039: localPos 不是 QWheelEvent 的成员"。
//
//    正确的对应关系是：
//      Qt5（任意小版本）：用 pos()
//      Qt6：用 position()
//
//    这里按"是否有 position()"来分：
//      Qt >= 5.14 也有 position()，但那是个 5.14 才加的接口，
//      为了兼容 5.12/5.9 这类更老的版本，Qt5 分支统一用 pos()。
//      （pos() 在 Qt5.15 下只是"废弃警告"，功能完全正常。）
// ---------------------------------------------------------------------------

inline QPointF wheelEventPos(const QWheelEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    // ★ 局部屏蔽 C4996（"QWheelEvent::pos 已废弃，请用 position()"）★
    //   这条警告是 MSVC 对 Qt5 头文件里 Q_DECL_DEPRECATED 标注的反应。
    //   在本函数里调用 pos() 是**刻意的**：Qt6 分支才用 position()。
    //   如果不屏蔽，每次全量构建都会刷出十几条同样的警告，
    //   真正需要关注的警告会被淹没 —— 警告疲劳比没有警告更危险。
    //
    //   只在 Qt5 分支里屏蔽，范围局限在这几行，不影响其他文件的检查。
#  if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4996)
#  endif
    return QPointF(e->pos());
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
#endif
}

inline QPointF wheelEventGlobalPos(const QWheelEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition();
#else
#  if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4996)
#  endif
    return QPointF(e->globalPos());
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
#endif
}

// ---------------------------------------------------------------------------
//  图像镜像 / 翻转
// ---------------------------------------------------------------------------
//  Qt5：QImage::mirrored(bool horizontal, bool vertical) —— 一次给两个方向
//  Qt6.0 ~ 6.8：mirrored() 仍然可用，只是被标记为"将在 6.13 移除"
//  Qt6.9+：新增了 flipped(Qt::Orientations)，语义更清楚
//
//  ★★ 这里有一个踩过的真坑（CI 上暴露的）★★
//
//    最初写成 `QT_VERSION >= QT_VERSION_CHECK(6,0,0)` 就走 flipped() ——
//    想当然地以为"Qt6 都改名叫 flipped 了"。实际上 **flipped() 是 6.9 才
//    引入的**：6.0~6.8 上根本没有这个成员，编译直接报
//      error C2039: 'flipped': is not a member of 'QImage'
//    而本地只装了 Qt5，这条分支从来没被编译过 —— 一路"绿灯"到 CI 才炸。
//
//    正确的分界是 **6.9**，不是 6.0；6.0~6.8 继续用 mirrored()（它还活着）。
//
//  ★ 为什么 6.9 分支要用两次 flipped() 串联 ★
//    flipped() 一次只接受一个方向，水平 + 垂直要调用两次。只调一次会
//    "静默地只翻一半" —— 编译通过、运行不报错，但结果错误。
//    本工程对翻转功能有逐像素对比的测试钉着这件事。
// ---------------------------------------------------------------------------

inline QImage mirrorImage(const QImage &src, bool horizontal, bool vertical)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QImage result = src;
    if (horizontal)
        result = result.flipped(Qt::Horizontal);
    if (vertical)
        result = result.flipped(Qt::Vertical);
    return result;
#else
    return src.mirrored(horizontal, vertical);
#endif
}

// ---------------------------------------------------------------------------
//  文本流编码
// ---------------------------------------------------------------------------
//  Qt5：QTextStream::setCodec("UTF-8")
//  Qt6：setCodec 被删除，改用 setEncoding(QStringConverter::Utf8)
// ---------------------------------------------------------------------------

inline void setUtf8(QTextStream &ts)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    ts.setEncoding(QStringConverter::Utf8);
#else
    ts.setCodec("UTF-8");
#endif
}

// ---------------------------------------------------------------------------
//  容器选择：QList 还是 QVector
// ---------------------------------------------------------------------------
//  ★ Qt5 的 QList<T> 与 QVector<T> **不等价**，这里统一说清楚 ★
//
//    Qt5 的 QList 是"指针数组 + 内联存储"的混合结构，凡是 T 的大小超过
//    指针长度的类型，元素都是**堆上单独分配**的，因此 QList：
//      · 没有 resize()
//      · 没有 (size, value) 构造
//      · 没有 data() / constData()（不保证连续）
//    Qt5 的 QVector 才是真正的连续数组，上述三个接口它都有。
//
//    Qt6 把两者统一了：QVector 变成 QList 的别名（连续内存）。
//    所以凡是上面三个接口要用到的地方，写 QVector 在 Qt5/Qt6 下**都能编**。
//
//  本工程的规则（以各文件里的实际注释为准）：
//    · 需要"预分配固定长度 + 下标随机访问 + 连续内存"的场合 → QVector
//      典型场景：像素缓冲、滤波器权重表、查找表、渲染临时缓冲
//    · 只是普通的对象列表（点列表、图层名列表等）→ QList
//
//  这条区分不是风格偏好：曾经因为对"像素缓冲"用了 QList 而报
//  "QList 没有成员 resize"，且报错位置在模板实例化处，离真正的原因很远。
// ---------------------------------------------------------------------------

namespace compat {

// 当前编译所用的 Qt 主版本号，日志与关于对话框中会显示
constexpr int qtMajorVersion()
{
    return QT_VERSION_MAJOR;
}

// 人类可读的 Qt 版本字符串，例如 "5.15.16"
inline QString qtVersionString()
{
    return QString::fromLatin1(qVersion());
}

} // namespace compat

} // namespace zl
