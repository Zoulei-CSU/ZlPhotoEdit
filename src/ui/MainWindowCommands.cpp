// ============================================================================
//  MainWindowCommands.cpp —— 各菜单命令的实现
// ----------------------------------------------------------------------------
//  本文件包含四类命令：
//    A. 文件生命周期（新建/打开/保存/另存为/关闭/未保存提示）
//    B. 编辑（撤销/重做/剪贴板/选区清空）
//    C. 调整与几何（去色/反相/旋转/翻转）
//    D. 图层（增删移合）
//    以及视图操作与快捷键派发。
//
//  ★ 参数化调整（亮度对比度 / 色相饱和度 / 曲线）另走一条流程 ★
//
//    它们不能"一键执行"，需要先收集参数、期间还要实时预览。
//    这一段的实现在文件末尾的 E 节，入口是 runAdjustment()。
//    与上面的骨架相比，它多了两步（打开对话框、预览），
//    但"只压一条 Command"和"不见效就不压栈"这两条依然不变。
//
//  ★ 命令实现的通用模式 ★
//
//    所有会修改图像的菜单命令都遵循同一个骨架：
//
//      1. requireDocument()      —— 没有文档就直接返回并提示
//      2. 记录操作前的状态        —— captureXxx()
//      3. 执行操作（调用 engine 或 Document 的方法）
//      4. 构造对应的 Command 并 push —— 撤销数据来自步骤 2
//
//    这个骨架的价值在于"不漏掉撤销"。如果一个命令执行了第 3 步却忘了
//    第 4 步，用户按 Ctrl+Z 会撤销掉**上一步**操作，而这一步的效果
//    永久留在图像上 —— 这是最难排查的一类 bug（表现为"撤销乱跳"）。
//
//    所以本文件里的每个命令都严格照这个骨架写，便于审查。
// ============================================================================

#include "compat/GlobalHotkey.h"
#include "compat/ScreenCapture.h"

#include "ui/MainWindow.h"
#include "ui/CanvasView.h"
#include "ui/ToolOptionsBar.h"
#include "ui/StatusBarWidget.h"
#include "ui/LayerPanel.h"
#include "ui/IconLibrary.h"
// ★ 必须完整定义 ★
//   closeDocumentTab / activateTab / closeEvent 都要调它的方法
//   （tabAt / removeDocumentTab / count / currentIndex…），
//   前置声明不够 —— 报错是 C2027「使用了未定义类型」。
#include "ui/DocumentTabs.h"
#include "ui/RulerWidget.h"
// ★ 界面主题 ★ setTheme() 要转调它（应用 + 落盘）。
#include "core/UiTheme.h"

// 调整对话框（Phase 4）。
// ★ 必须包含完整定义而不能只前置声明 ★
//   本文件里要 new 它们并调用虚函数，前者需要完整类型。
#include "ui/AdjustmentDialog.h"
#include "ui/BrightnessContrastDialog.h"
#include "ui/HueSaturationDialog.h"
#include "ui/CurvesDialog.h"
#include "ui/LevelsDialog.h"
#include "ui/ImageSizeDialog.h"
#include "ui/CanvasSizeDialog.h"
#include "ui/GridSettingsDialog.h"
#include "ui/RotateImageDialog.h"

#include "core/DocumentManager.h"
#include "core/RecoveryManager.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/ClipboardData.h"

#include "commands/CommandStack.h"
#include "commands/LayerCommand.h"
#include "commands/SelectionCommand.h"
#include "commands/ImageCommand.h"
#include "commands/GeometryChangeCommand.h"

#include "engine/Filters.h"
#include "engine/Geometry.h"
#include "engine/IcoWriter.h"
#include "engine/ImageMeta.h"
#include "engine/RulerMetrics.h"
#include "engine/Transform.h"

#include "tools/ToolManager.h"
#include "tools/AbstractTool.h"
#include "tools/SelectionTool.h"
#include "tools/ShapeTool.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
// 屏幕截图要用 QGuiApplication::primaryScreen() 取逻辑 DPI（见 captureScreenNow）
#include <QGuiApplication>
#include <QScreen>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QClipboard>
#include <QSettings>
#include <QImageWriter>
#include <QImageReader>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPrintDialog>
#include <QPageSize>
#include <QPageLayout>
#include <QDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>   // 「打开为图层」的三选一对话框要拿 addButton 的返回值比较
#include <QScopedPointer>
// ★ QScopedValueRollback ★
//   它是"进入作用域置位、离开自动还原"的 RAII 守卫。多标签的增/摘页
//   需要它给 m_switchingTabs 加锁 —— 而"中途 return"在下面有七八处，
//   手写 set/restore 早晚会漏掉一条 return 路径，
//   症状是"那次操作之后切页再也不工作了"（标志位永久为真）。
#include <QScopedValueRollback>
// qFloor / qCeil 在 QtMath 里，不在 QtGlobal 里 ——
// 少了这个头会报"C3861 找不到标识符 qFloor"，而报错信息完全看不出
// 是缺 include（看起来像拼错了函数名）。
#include <QtMath>
#include <QColorDialog>

// ★ 面板来的批量图层操作要用排序，必须显式包含 ★
//   Qt 的头文件在多数平台上会顺带拉进 <algorithm>，但那是"实现细节"，
//   换个 Qt 版本或编译顺序就可能消失 —— 这类"别人替我包含了"的依赖，
//   报错时表现为"找不到 std::sort"，完全看不出是缺 include。
#include <algorithm>
#include <functional>
#include <QPainter>
#include <QPalette>
#include <QBuffer>
#include <QTimer>
#include <QFile>
#include <QWidget>
#include <QDebug>

#include <utility>   // std::swap

namespace zl {

// ============================================================================
//  内部常量
// ============================================================================
namespace {

// 新建文档的默认尺寸。
//  1200×800 而不是 1920×1080：
//  默认值应该"轻"到能瞬时创建，同时"大"到能容纳几张照片的拼接。
//  1200×800 的 ARGB32 是 3.8MB，创建几乎无感。
constexpr int kDefaultNewWidth  = 1200;
constexpr int kDefaultNewHeight = 800;

// 单张图像的最大像素数上限。
//  1 亿像素 = 400MB（ARGB32）。超过这个量级的操作（滤镜、整层快照撤销）
//  都会让程序变得不可用，而且撤销栈一旦存两份就是 800MB。
//  与其让用户经历"操作到一半内存耗尽崩溃"，不如打开时就明确拒绝。
//  这是"提前失败"（fail fast）原则的具体应用。
constexpr qint64 kMaxPixelCount = 100LL * 1024 * 1024;

// ---------------------------------------------------------------------------
//  一个文档是否有未保存的修改
// ---------------------------------------------------------------------------
//
//  ★ 判据必须**只有一份** ★
//
//    "这个文档改了没保存"这件事在三个地方要被问到：
//      ① 关一页时（maybeSave）
//      ② 关窗口时（maybeSaveAll，逐页问）
//      ③ 标签上要不要画 *
//    三处若各写一遍，迟早会出现"标签带着 *，但关闭时不问保存"这种
//    自相矛盾的状态 —— 而用户只有真的丢了东西才会发现。
//
//  ★ 为什么以撤销栈的 clean 状态优先 ★
//    Document::isModified() 只是一个手动维护的标志位，容易与实际脱节。
//    而 QUndoStack 的 clean 状态是**自动跟踪**的：用户撤销回到保存点时
//    它会自己变回 clean。
//    具体场景：用户画了一笔（modified=true），按 Ctrl+Z 撤销回去 ——
//    此时内容与保存时完全一致，不该再提示保存。用 isModified 会误提示。
bool needsSave(const Document *doc, const CommandStack *stack)
{
    if (!doc)
        return false;
    if (stack)
        return !stack->isClean();
    return doc->isModified();
}

// PNG 的默认压缩级别（0-100，值越大压缩越强但越慢）。
//  90 是"文件够小、保存够快"的平衡点。100 会让 4000×3000 的图
//  保存耗时从 1.5 秒涨到 4 秒，而体积只小 3%。
constexpr int kDefaultPngQuality = 90;

// JPEG 的默认质量。
//  92 是肉眼几乎看不出压缩痕迹的下限。低于 85 会在天空、皮肤这类
//  平滑渐变区域出现明显的块状伪影。
constexpr int kDefaultJpegQuality = 92;

} // anonymous namespace

// 未命名文档的全局计数器（让标题显示"未命名-1"、"未命名-2"…）
static int g_untitledCounter = 1;

// ============================================================================
//  辅助
// ============================================================================

bool MainWindow::requireDocument()
{
    if (m_doc)
        return true;

    // 没有文档时给出明确提示，而不是静默失败。
    // 静默失败会让用户以为"这个菜单项坏了"。
    if (m_statusBar)
        m_statusBar->showMessage(tr("Create or open an image first"), 3000);

    return false;
}

QImage MainWindow::captureActiveLayerImage() const
{
    if (!m_doc)
        return QImage();

    const Layer *layer = m_doc->activeLayer();
    if (!layer)
        return QImage();

    // ★ 必须 copy() ★
    //   拿到的是"操作前的整层快照"，它要一直活到用户撤销时才使用。
    //   如果不 copy，这个 QImage 与图层当前的位图共享内存 ——
    //   后续对图层的修改虽然会触发 Qt 的写时复制（从而分离），
    //   但"如果修改走了 bits() 等非 Qt API 路径"就会污染快照。
    //   主动 copy 才是有保证的做法（成本：一次 48MB 拷贝，约 10ms）。
    //
    //   另一个原因：QImage::image() 返回 const 引用，直接赋值给
    //   QImage 只是引用计数 +1。而 ImageCommand 需要的是"冻结的历史状态"。
    return layer->image().copy();
}

void MainWindow::pushActiveLayerImageCommand(const QImage &before,
                                             const QImage &after,
                                             const QString &text)
{
    if (!m_doc || !m_commandStack)
        return;

    if (before.isNull() || after.isNull())
        return;

    if (before.size() != after.size()) {
        // 尺寸不同说明调用方式有问题（整层变换不应该改变尺寸）。
        // 不静默接受 —— 那会在撤销时写入一张尺寸不对的图，导致画面错乱。
        qWarning() << "pushActiveLayerImageCommand: before/after 尺寸不一致，已忽略。"
                   << before.size() << after.size();
        return;
    }

    auto *cmd = new ImageCommand(m_doc, m_doc->activeLayerIndex(),
                                 before, after, text);
    m_commandStack->push(cmd);
}

// ============================================================================
//  A. 文件生命周期
// ============================================================================

void MainWindow::newDocument()
{
    // ★ 多标签之后，这里**不再需要未保存确认** ★
    //
    //   旧实现的第一行是 maybeSave()，因为"新建"会**关掉**当前文档。
    //   现在新建只是**多开一页**，原文档原封不动 —— 没有任何东西会丢，
    //   弹一句"要保存吗"就是纯粹的打扰。
    //
    //   这正是多标签带来的体验改进之一：整个"新建/打开会丢东西"
    //   的顾虑类别消失了，连同它的那一段确认代码一起。
    //   真正需要确认的只剩两处：关一页、关窗口。

    // ★ Phase 1 用固定的默认尺寸 ★
    //   Phase 7 会加一个"新建文档对话框"（尺寸 + 背景色 + 预设）。
    //   现在直接建一个够用的默认画布 —— 用户可以随后用"图像大小"调整，
    //   而先把对话框做出来会推迟其他更有价值的功能。
    Document *doc = m_docManager->createDocument(
        QSize(kDefaultNewWidth, kDefaultNewHeight),
        Qt::transparent);

    if (!doc) {
        QMessageBox::warning(this, tr("New Failed"),
                             tr("Cannot create the document: %1").arg(m_docManager->lastError()));
        return;
    }

    // 设置显示名。空文件路径 + displayName 会用 "未命名-N" 的形式。
    doc->setModified(false);

    // ★ 开一页并切到它 ★
    //   顺序不能反：createDocument 只是"把文档装进管理器"，
    //   而"页"是 UI 那一侧的东西 —— 由 attachDocumentTab 建立
    //   （它会一并建好这一页的撤销栈与画布）。
    attachDocumentTab(doc);

    // 适合窗口
    if (m_canvas)
        m_canvas->zoomToFit();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Created a blank %1 × %2 image")
                                     .arg(kDefaultNewWidth)
                                     .arg(kDefaultNewHeight), 3000);
}

void MainWindow::openDocument()
{
    // ★ 同样不再需要 maybeSave() ★ —— "打开"现在是新开一页，见 openDocumentFromPath。

    QSettings settings;
    // 记住上次打开的目录 —— 每次都从"我的文档"开始找很烦
    const QString lastDir = settings.value(QStringLiteral("files/lastOpenDir"),
                                           QStandardPaths::writableLocation(
                                               QStandardPaths::PicturesLocation))
                                .toString();

    // ★ 过滤器覆盖 Qt 支持的全部读格式 ★
    //   不用写死 "*.png *.jpg" —— QImageReader 能列出当前安装支持的全部格式，
    //   包括用户系统上额外安装的图片格式插件（如 WebP、HEIF）。
    //   写死的话，用户装了 WebP 插件却打不开 WebP，会以为程序有问题。
    QStringList filters;
    filters << tr("All Supported Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.ico *.ppm *.xbm *.xpm)");
    filters << tr("PNG Images (*.png)");
    filters << tr("JPEG Images (*.jpg *.jpeg)");
    filters << tr("Bitmaps (*.bmp)");
    filters << tr("TIFF Images (*.tif *.tiff)");
    filters << tr("WebP Images (*.webp)");
    filters << tr("All Files (*)");

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Image"), lastDir, filters.join(QStringLiteral(";;")));

    if (path.isEmpty())
        return;

    openDocumentFromPath(path);
}

bool MainWindow::openDocumentFromPath(const QString &path)
{
    if (path.isEmpty())
        return false;

    // ★ 这里**没有** maybeSave() 了 ★
    //
    //   旧实现的第一步是先问"当前文档要不要保存"，最后再
    //   closeDocument(旧索引) —— 那是单文档时代的语义：
    //   "打开"等于"用新图替换掉当前这张"。
    //
    //   多标签之后，"打开"是**新开一页**，当前文档原封不动。
    //   于是那段确认代码既多余、又有害：用户完全可以在两个标签里
    //   同时持有两张图，没有任何东西会被覆盖。
    //
    //   ★ 注意这不等于"永远不会丢东西" ★
    //     需要确认的地方仍然有两处：关一页、关窗口。它们各自
    //     走 maybeSave / maybeSaveAll —— 一处都没少。

    // ── 已经打开过同一个文件？直接切过去 ──
    //
    //  ★ 为什么不新开一页 ★
    //    用户从"最近打开"里点了一个已经在标签里的文件，
    //    期望是"切到那个标签"，而不是多出一个内容相同但独立的副本。
    //    多副本会让"在两个标签里各改一遍、保存时后者覆盖前者"成真 ——
    //    而且用户在保存前完全不会意识到自己有两份。
    //
    //  ★ 代价：没有"重新载入"了 ★
    //    旧实现在只有一个文档时允许重新加载同一个文件（相当于丢弃修改
    //    重新开始）。现在这个动作请用"关掉那一页再打开"完成 ——
    //    语义更明确（关掉时会问要不要保存），而且不会有"我按了打开，
    //    结果我的修改无声无息地没了"这种事。
    const int existing = m_docManager->indexOfPath(path);
    if (existing >= 0) {
        Document *doc = m_docManager->documentAt(existing);
        const int tabIndex = doc && m_tabs ? m_tabs->indexOfDocument(doc) : -1;

        if (tabIndex >= 0) {
            {
                const QScopedValueRollback<bool> guard(m_switchingTabs, true);
                m_tabs->setCurrentIndex(tabIndex);
            }
            activateTab(tabIndex);

            addRecentFile(path);

            if (m_statusBar) {
                m_statusBar->showMessage(
                    tr("%1 is already open - switched to its tab")
                        .arg(QFileInfo(path).fileName()), 3000);
            }
            return true;
        }
    }

    // ── 打开前的元数据预检 ──
    //
    //  ★ 为什么先读元数据而不直接读图 ★
    //    readMetadata 只解析文件头（几毫秒），能立刻拿到尺寸。
    //    如果直接 loadImageWithMetadata 去解一张 2 亿像素的图，
    //    程序会因为内存耗尽而卡死或崩溃，用户连一句提示都看不到。
    //    先检查再加载，能把"崩溃"变成"一句明确的拒绝"。
    ImageMetadata meta = readMetadata(path);
    if (meta.size.isEmpty()) {
        QMessageBox::warning(this, tr("Open Failed"),
            tr("Cannot read the image info:\n%1\n\n"
               "Possible causes: the file is corrupted, the format is "
               "unsupported, or there is no read permission.").arg(path));
        return false;
    }

    const qint64 pixelCount = qint64(meta.size.width()) * meta.size.height();
    if (pixelCount > kMaxPixelCount) {
        const double mp = pixelCount / 1000000.0;
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, tr("Image Too Large"),
            tr("This image is %1 × %2 (about %3 megapixels),\n"
               "above the recommended limit (%4 megapixels).\n\n"
               "Opening it may run out of memory and become very slow.\n\n"
               "Try to open it anyway?")
                .arg(meta.size.width()).arg(meta.size.height())
                .arg(mp, 0, 'f', 1)
                .arg(kMaxPixelCount / 1000000),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return false;
    }

    // ── 真正加载 ──
    ImageMetadata loadedMeta;
    const QImage image = loadImageWithMetadata(path, &loadedMeta);

    if (image.isNull()) {
        QMessageBox::warning(this, tr("Open Failed"),
            tr("Cannot load the image data:\n%1\n\n"
               "The file may be corrupted, or the image data uses an "
               "unsupported encoding.").arg(path));
        return false;
    }

    // ★ 新开一页 ★
    //
    //  ★ 为什么"新建一份文档"而不是"复用某个文档" ★
    //    复用需要一套"把旧文档的内容整体替换掉"的逻辑（所有图层、
    //    选区、撤销栈全部重置）。这套逻辑与"新建文档"几乎完全重合，
    //    却多了一堆"忘记重置某个字段"的风险 ——
    //    例如忘了重置选区：用户框选一小块后打开大图，
    //    会发现"画笔只在左上角能用"。
    //    多标签之后"每张图一份文档"本来就是模型本身，连选择都没有了。
    //
    //  ★ 建立动作已收敛到 createImagePage() ★
    //    屏幕截图那边要做完全相同的事（一张图 → 一页 + 切过去）。
    //    两份实现的话，"图层 0 的名字从哪来""路径必须在开页之前设好"
    //    这类规则迟早只更新一份 —— 于是这里只传参数，不重复那段顺序。
    ImagePageSpec spec;
    spec.image     = image;
    spec.filePath  = path;
    spec.layerName = QFileInfo(path).fileName();   // 图层面板里显示"photo.jpg"比"图层 1"有用
    spec.dpiX      = loadedMeta.dpiX;
    spec.dpiY      = loadedMeta.dpiY;

    Document *doc = createImagePage(spec);
    if (!doc) {
        QMessageBox::warning(this, tr("Open Failed"),
            tr("Cannot create the document: %1").arg(m_docManager->lastError()));
        return false;
    }

    addRecentFile(path);

    // 记住目录
    QSettings settings;
    settings.setValue(QStringLiteral("files/lastOpenDir"), QFileInfo(path).absolutePath());

    if (m_statusBar) {
        m_statusBar->showMessage(
            tr("Opened %1 (%2 × %3, %4)")
                .arg(QFileInfo(path).fileName())
                .arg(image.width())
                .arg(image.height())
                .arg(loadedMeta.formatName.isEmpty() ? tr("Unknown Format") : loadedMeta.formatName),
            4000);
    }

    return true;
}

// ============================================================================
//  createImagePage —— "一张图 → 一页"的唯一实现
// ============================================================================
//
//  两个调用者：openDocumentFromPath()（从磁盘读出来的图）与
//  captureScreenNow()（从屏幕抓来的图）。两者的差别只有"图从哪来"与
//  "有没有文件路径"，而**"怎么变成一页"完全一样**：
//
//    ① 建一个与图像同尺寸的文档（透明底）
//    ② 把图写进图层 0
//    ③ 设图层名 —— 图层面板里一眼看出这层是什么
//    ④ ★ 设路径 / 显示名提示：必须在**开页之前** ★
//       标签标题是 DocumentTabs::tabTitle 现算的（filePath 或 titleHint），
//       顺序反了会先显示"未命名-2"再被刷新成"photo.jpg"：虽然只是闪一下，
//       但那是"两处真相"的味道 —— 任何"只读一次标题"的地方都会读到错的。
//    ⑤ markDirty() —— 让合成缓存看到新写的像素。
//       新建文档的缓存本来就是空的，这句看似多余；但它是"改了像素就必须
//       说一声"这条纪律的一部分 —— 省掉它，将来上面那句 setImage 被挪到
//       别处时就会静默留下一片空白缓存。
//    ⑥ setModified(false) —— 刚打开 / 刚截的图不算"已修改"，
//       否则关它一下都要问"要不要保存"（撤销栈是 clean 的，这里说的是
//       那个手动标志位，两者必须都表示"干净"）
//    ⑦ 开一页并切过去（新页自带一个干净的撤销栈 —— 旧实现在这里写
//       m_commandStack->clear()，现在每页一个栈，那句"清空"连同它要
//       解决的问题一起消失了）

Document *MainWindow::createImagePage(const ImagePageSpec &spec)
{
    if (!m_docManager || spec.image.isNull())
        return nullptr;

    Document *doc = m_docManager->createDocument(spec.image.size(), Qt::transparent);
    if (!doc)
        return nullptr;      // lastError() 里已经写明了原因（尺寸超限等）

    if (doc->layerCount() > 0) {
        if (Layer *layer = doc->layers().layer(0)) {
            layer->setImage(spec.image);
            if (!spec.layerName.isEmpty())
                layer->setName(spec.layerName);
        }
    }

    doc->setFilePath(spec.filePath);       // 空 = 没有磁盘来源（屏幕截图）
    doc->setTitleHint(spec.titleHint);     // 无路径时的显示名（如"屏幕截图-1"）

    if (spec.dpiX > 0.0 && spec.dpiY > 0.0)
        doc->setDpi(spec.dpiX, spec.dpiY);

    doc->markDirty();
    doc->setModified(false);

    attachDocumentTab(doc);
    return doc;
}

// ============================================================================
//  屏幕截图（compat/ScreenCapture + compat/GlobalHotkey）
// ============================================================================
//
//  ★ 一条功能，两个"环境依赖" ★
//    抓屏依赖真实显示器、全局热键依赖操作系统 API —— 两样在自检环境里
//    都不存在。所以这条功能被切成三段，每段都能单独验：
//
//      grabScreen()        compat 层：抓不到就返回空图（契约见那边头文件）
//      createImagePage()   建页（上面那个，已经是"图 → 一页"的唯一实现）
//      captureScreenNow()  把两段接起来 + 报结果          ← 本函数
//
//    自检用 setScreenGrabber() 把第一段换成一张造好的图，于是
//    "菜单立即截图 / 延迟截图 / 全局热键"三条入口全都能在 offscreen 上
//    端到端跑通 —— 它们共用下面这一个函数，不存在"测的是另一条路"。
//
//  ★ 抓不到屏幕时必须"什么都不做 + 说清楚为什么" ★
//    没有显示器、会话被锁、平台拒绝……都不是程序 bug，但它们的表现
//    全都是"按了没反应"。所以失败路径一定有提示，绝不留白。
//
//  ★ 提示的形态：状态栏 + 模态框，两者都要 ★
//    状态栏是"此刻发生了什么"的即时反馈，但它一闪而过、还可能被别的
//    消息顶掉（延迟截图到点时状态栏正写着"3 秒后截图…"）。
//    Wayland 这类"这条路在当前桌面上根本走不通"的失败尤其不能靠状态栏：
//    用户点完菜单什么都没发生，只会认为功能坏了。
//    所以交互路径**一定要弹模态框**，把"为什么"和"接下来怎么做"写全。

// ============================================================================
//  抓屏失败的文案（与"怎么显示"分开，便于自检断言）
// ============================================================================

QString MainWindow::captureFailureTitle()
{
    return tr("Screenshot Unavailable");
}

QString MainWindow::captureFailureText(CaptureBlock block)
{
    // ★ 分两种说法，因为"接下来该怎么做"完全不同 ★
    //   Wayland 会话下这不是"这一次没抓到"，而是**这条路在当前桌面
    //   环境里根本走不通**（合成器不允许客户端读屏幕上不属于自己的
    //   像素，Qt 的 grabWindow() 在那里恒返回空图）。此时说"可能没有
    //   显示器"是误导 —— 用户明明开着屏幕。
    //   要告诉他改用系统截图键，再 Ctrl+V 粘进来：粘贴走剪贴板，
    //   Wayland 下完全正常，不需要任何额外授权。
    //
    // ★ 这两句与状态栏用的是**同一份源文本** ★
    //   另外编一句更短的放在状态栏，等于同一件事要维护两份译文 ——
    //   而译文散开之后，改一处忘一处几乎是必然的。
    if (block == CaptureBlock::Wayland) {
        return tr("This Wayland session does not allow applications to read the "
                  "screen, so the built-in capture cannot work. Take a screenshot "
                  "with the system shortcut, then press Ctrl+V to paste it here "
                  "as a new layer.");
    }
    return tr("Screenshot failed: no screen content captured "
              "(no display may be available)");
}

void MainWindow::setScreenCaptureDialogsEnabled(bool enabled)
{
    m_captureDialogs = enabled;
}

bool MainWindow::captureScreenNow()
{
    // ── ① 取图：默认真的抓屏，自检/截图工具可以换成造好的图 ──
    const QImage shot = m_screenGrabber ? m_screenGrabber() : grabScreen();

    if (shot.isNull()) {
        // 契约③（空图 = 抓不到）在这里翻译成人话。
        const CaptureBlock block = screenCaptureBlock();

        // ── 先写状态栏：这个是**同步**的，调用方返回时就已经在屏幕上 ──
        //   与弹框共用 captureFailureText() 的同一份源文本：同一件事只有
        //   一份译文要维护，也就不会出现"状态栏和弹框说法不一致"。
        if (m_statusBar)
            m_statusBar->showMessage(captureFailureText(block), 20000);

        // ── 再排队弹框 ──
        //
        // ★ 为什么不是在这里直接 exec() ★
        //   这个函数有两个调用点会把它叫在"窗口正被最小化"的时刻：
        //   延迟截图到点（onCaptureTimerFired，抓完才 showNormal）
        //   和全局热键（用户按的时候程序本来就在后台）。
        //   直接 exec() 的话模态框会挂在一个最小化 / 隐藏的父窗口上 ——
        //   要么根本看不见，要么一出来就抢焦点把用户正在做的事打断。
        //   排队到下一轮事件循环，调用链（含"抓完还原窗口"）先跑完，
        //   弹框时就有一个正常显示的父窗口可以依附。
        //
        //   返回值仍然是**同步**的 false：调用方不用等用户点确定。
        if (m_captureDialogs && !m_captureFailureDialogPending) {
            m_captureFailureDialogPending = true;
            QTimer::singleShot(0, this, [this, block] {
                m_captureFailureDialogPending = false;

                // 父窗口最小化时，模态框只会出现在任务栏里 —— 用户看不到，
                // 于是"点了没反应"照旧。先把它叫回来（这一步是必要的打断：
                // 不打断就永远没机会告诉他为什么没反应）。
                if (isMinimized())
                    showNormal();
                raise();
                activateWindow();

                QMessageBox::warning(this, captureFailureTitle(),
                                     captureFailureText(block));
            });
        }
        return false;
    }

    // ── ② 名字：序号只增不减（见 m_screenshotSeq 的说明）──
    const QString hint = tr("Screenshot-%1").arg(++m_screenshotSeq);

    ImagePageSpec spec;
    spec.image     = shot;
    spec.layerName = tr("Screenshot");
    spec.titleHint = hint;

    // ★ dpi 取主屏的**逻辑** DPI ★
    //   抓回来的图是设备像素：150% 缩放的屏幕上，1920 逻辑像素宽的一屏
    //   给出 2880 个像素。配 192 的逻辑 DPI（96 × 1.5）算出来的物理尺寸
    //   才等于屏幕上那个尺寸 —— 否则打印预览会把一张 24 寸屏的截图
    //   显示成 40 寸。
    //
    //   多屏 dpr 不同时这只是近似值。dpi 只影响打印尺寸与标尺单位换算，
    //   不值得为它再引入一套"到底是哪块屏"的判断（要紧的是像素本身）。
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        spec.dpiX = primary->logicalDotsPerInch();
        spec.dpiY = primary->logicalDotsPerInch();
    }

    Document *doc = createImagePage(spec);
    if (!doc) {
        if (m_statusBar)
            m_statusBar->showMessage(
                tr("Screenshot failed: cannot create the document (%1)")
                    .arg(m_docManager ? m_docManager->lastError() : QString()),
                6000);
        return false;
    }

    // 全新的页：缩放到适合窗口，用户一眼看到全貌
    if (m_canvas)
        m_canvas->zoomToFit();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Captured the screen at %1 × %2 and opened it as a new tab")
                                     .arg(shot.width()).arg(shot.height()),
                                 4000);
    return true;
}

void MainWindow::scheduleScreenCapture(int delayMs)
{
    if (delayMs < 0)
        delayMs = 0;

    // ★ "是不是我们把它最小化的"只在**新排一次**时判定 ★
    //   ★ 这一条踩过：连点两次"延迟 3 秒"会让窗口在没有任何还原的情况下
    //     留在最小化状态 ★
    //     第二次进来时窗口正因为第一次而处于最小化，再判一次得到的是
    //     false，于是 m_captureMinimizedSelf 从"是我们干的"被抹成"不是
    //     我们干的"——到点抓完就没人负责还原，程序看起来消失了（只能在
    //     任务栏里找回来）。
    //     判据是"当时有没有一次在排队"，而不是"窗口现在是不是最小化的"：
    //     后者把"我们已经接管了窗口状态"这个事实丢掉了。
    const bool freshRequest = (m_captureDelayMs < 0);

    if (freshRequest)
        m_captureMinimizedSelf = !isMinimized();

    m_captureDelayMs = delayMs;

    if (m_captureTimer) {
        m_captureTimer->setSingleShot(true);
        // ★ start() 会重置正在跑的定时器 ★ —— 这正是"重复点 = 重新计时"。
        //   不做成"每个请求一个定时器"：那会攒出好几张图，而用户连点三次
        //   "延迟 3 秒"想要的显然只有一张。
        m_captureTimer->start(delayMs);
    }

    // ── 先把本窗口最小化，再等 ──
    //   ★ 记下"是不是我们干的" ★
    //     用户本来就把它最小化了的话，抓完不该把窗口弹到他脸上 ——
    //     那等于替他改了窗口状态（而他可能正忙着别的）。
    if (m_captureMinimizedSelf)
        showMinimized();

    if (m_statusBar)
        m_statusBar->showMessage(
            tr("Screenshot in %1 seconds: this window is minimized, switch to the target screen")
                .arg(delayMs / 1000.0, 0, 'g', 3),
            delayMs + 1000);
}

void MainWindow::onCaptureTimerFired()
{
    m_captureDelayMs = -1;

    const bool restoreWindow = m_captureMinimizedSelf;
    m_captureMinimizedSelf = false;

    // ── 先抓、后还原 ──
    //   反过来的话，恢复出来的窗口会**出现在截下来的图里** ——
    //   而"延迟截图"的全部意义就是不让自己挡住目标画面。
    captureScreenNow();

    if (restoreWindow) {
        // ★ 失败也要还原 ★
        //   只在成功时还原的话，用户面对的是一个"点完就消失"的程序，
        //   只能去任务栏里把它找回来 —— 而原因只是一次抓屏失败。
        showNormal();
        raise();
        activateWindow();
    }
}

void MainWindow::setScreenGrabber(std::function<QImage()> grabber)
{
    m_screenGrabber = std::move(grabber);
}

// ============================================================================
//  openDocumentPaths —— 批量打开（命令行参数 / 拖入多个文件）
// ============================================================================
//
//  ★ 为什么要收敛成一个函数 ★
//    "给我一串路径，每个开一页"在命令行与拖放两个入口上是**同一件事**。
//    各写一份的话，"已经打开过的文件怎么办"这类规则必然只有一份会被
//    更新到位，而两份的差异只有真的拖多个文件进来才看得到。
//
//  ★ 返回值语义 ★
//    成功开页（或切到已有页）的数量。构造函数用它决定
//    "命令行参数全都无效时要不要回落到一个空白文档"。

int MainWindow::openDocumentPaths(const QStringList &paths)
{
    int opened = 0;

    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        if (openDocumentFromPath(path))
            ++opened;
    }

    return opened;
}

// ============================================================================
//  打开为图层
// ============================================================================
//
//  把另一张图片整幅作为一个新图层加进**当前文档**。
//
//  ★ 它为什么能替代多文档 ★
//    "从图 B 框选一块贴到图 A"这个需求，缺的从来不是"复制 / 粘贴"
//    （复制走系统剪贴板、粘贴为新图层早已实现），而是**同时看到两张图**。
//    本功能把 B 整幅变成一个图层，于是"跨文档"问题退化成"跨图层"问题 ——
//    选区、反选、删除、移动这些图层内操作全都是现成的。
//    代价是多一步（在该层上框选后反选 + 删除），换来零架构改动。
//
//  ★ 与「打开」的区别必须清楚 ★
//    「打开」= 换文档（旧文档被关闭，撤销栈清空，标题/最近文件都变）；
//    「打开为图层」= 只往当前文档里加一层（文档没换，撤销栈连续，
//    标题与最近文件都不变）。两个动作的破坏力差得很远，
//    所以它们必须是两个菜单项，不能合并。
//
//  ★ 尺寸适配是这里唯一的决策点 ★
//    导入图 ≤ 画布：直接居中放进去。
//    导入图 >  画布：**必须问用户**。默认扩大画布（不丢内容），
//    也可以选择裁剪。不问就裁 = 悄悄丢用户的图；
//    不问就扩 = 悄悄改用户的画布尺寸。两者都不能默认做。
// ============================================================================

void MainWindow::openAsLayer()
{
    if (!requireDocument() || !m_commandStack)
        return;

    // ── 选文件 ──
    QSettings settings;
    // 与「打开」共用同一个"上次目录" —— 用户刚在哪找过图，多半还去那里找
    const QString lastDir = settings.value(QStringLiteral("files/lastOpenDir"),
                                           QStandardPaths::writableLocation(
                                               QStandardPaths::PicturesLocation))
                                .toString();

    QStringList filters;
    filters << tr("All Supported Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.ico *.ppm *.xbm *.xpm)");
    filters << tr("PNG Images (*.png)");
    filters << tr("JPEG Images (*.jpg *.jpeg)");
    filters << tr("Bitmaps (*.bmp)");
    filters << tr("TIFF Images (*.tif *.tiff)");
    filters << tr("WebP Images (*.webp)");
    filters << tr("All Files (*)");

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open as Layer"), lastDir, filters.join(QStringLiteral(";;")));

    if (path.isEmpty())
        return;

    // ★ 选完文件就交棒 ★
    //   后面全部逻辑都在 importImageAsLayer() 里。拆开的唯一理由见
    //   MainWindow.h：QFileDialog 会让自动化环境卡死，而"导入"这件事
    //   本身必须能被脚本驱动（截图工具就靠它），所以干活的那一半
    //   必须能脱离 UI 被调用 —— 与 TextTool::placeText() 同一个套路。
    importImageAsLayer(path, GrowPolicy::Ask);
}

// ============================================================================
//  importImageAsLayer —— 「打开为图层」的真正实现
// ============================================================================
//
//  ★ 它同时是产品功能与自动化入口 ★
//    人走的路径：openAsLayer() 弹框选文件 → 调本函数（policy = Ask）
//    脚本走的路径：直接调本函数（policy = Grow / Crop，不弹任何框）
//    两条路径共用同一份"预检 → 加载 → 尺寸决策 → 执行"，所以被人眼
//    验证过的行为与自检断言的行为**是同一条代码**，不会分叉。
//
//  ★ policy 存在的意义 ★
//    导入图比画布大时必须做决定：扩大画布（不丢图，但改了画布尺寸），
//    还是裁剪（保住画布，但丢图的一部分）。两个都不是能替用户默认做的
//    选择，所以要问。policy 就是"谁来回答这个问题"：
//      Ask        —— 弹三选一框问用户（交互式）
//      Grow/Crop  —— 调用方**替用户**回答了（自动化没有人可问，
//                    弹框 = 进程永久挂起）
//    ★ 绝不把"没人回答"默认为裁剪 ★
//    那等于在无人值守时静默丢掉用户图片的一部分。
//
//  ★ 自动化模式下本函数**不显示任何模态框** ★
//    不只是"要不要扩画布"那个询问，连"读不出图像""加载失败"
//    这类失败汇报也不弹 —— 一个损坏的文件就足以把脚本挂死。
//    这一点由下面的 reportFailure 统一保证，而不是靠每处记得判断。
// ============================================================================
bool MainWindow::importImageAsLayer(const QString &path, GrowPolicy policy)
{
    if (!m_doc || !m_commandStack || path.isEmpty())
        return false;

    // ★ 失败汇报的唯一出口 ★
    //   交互模式：弹框（用户必须看见，否则他不知道为什么没反应）
    //   自动化模式：只写状态栏（弹框会把无人值守的进程挂死）
    //   把这条判断收进一个 lambda，是为了让"自动化不弹框"成为
    //   一个**结构性**保证 —— 将来新增失败分支时忘了判断也不会漏，
    //   因为它根本没有 QMessageBox 可写。
    const auto reportFailure = [this, policy](const QString &title,
                                              const QString &text) {
        if (policy == GrowPolicy::Ask) {
            QMessageBox::warning(this, title, text);
        } else if (m_statusBar) {
            m_statusBar->showMessage(text, 6000);
        }
    };

    // ── 元数据预检（与 openDocumentFromPath 走同一道闸门）──
    //
    // ★ 为什么这里也要预检 ★
    //   导入并不比"打开"安全：一张 2 亿像素的图同样会先申请几 GB 内存。
    //   把"崩溃"变成"一句明确的拒绝"的成本只有一次函数调用。
    ImageMetadata meta = readMetadata(path);
    if (meta.size.isEmpty()) {
        reportFailure(tr("Import Failed"),
            tr("Cannot read the image info:\n%1\n\n"
               "Possible causes: the file is corrupted, the format is "
               "unsupported, or there is no read permission.").arg(path));
        return false;
    }

    const qint64 pixelCount = qint64(meta.size.width()) * meta.size.height();
    if (pixelCount > kMaxPixelCount) {
        const double mp = pixelCount / 1000000.0;

        // ★ 自动化模式：直接拒绝，不弹框 ★
        //   交互时把"崩溃"变成"一句明确的拒绝"是对的；自动化时没有
        //   第二个人来点那个"确定"，弹框等于把脚本挂死。宁可让脚本
        //   拿到 false 去报错，也不要让脚本把机器的内存撑爆。
        if (policy != GrowPolicy::Ask) {
            reportFailure(tr("Image Too Large"),
                tr("Import rejected: the image is about %1 megapixels, above the limit (%2 megapixels)")
                    .arg(mp, 0, 'f', 1).arg(kMaxPixelCount / 1000000));
            return false;
        }

        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, tr("Image Too Large"),
            tr("This image is %1 × %2 (about %3 megapixels),\n"
               "above the recommended limit (%4 megapixels).\n\n"
               "Importing it may run out of memory and become very "
               "slow.\n\n"
               "Try to import it anyway?")
                .arg(meta.size.width()).arg(meta.size.height())
                .arg(mp, 0, 'f', 1)
                .arg(kMaxPixelCount / 1000000),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return false;
    }

    // ── 真正加载 ──
    ImageMetadata loadedMeta;
    const QImage image = loadImageWithMetadata(path, &loadedMeta);

    if (image.isNull()) {
        reportFailure(tr("Import Failed"),
            tr("Cannot load the image data:\n%1\n\n"
               "The file may be corrupted, or the image data uses an "
               "unsupported encoding.").arg(path));
        return false;
    }

    // ── 尺寸适配决策 ──
    const QSize canvasSize = m_doc->canvasSize();
    const QSize imageSize  = image.size();

    const bool fits = (imageSize.width()  <= canvasSize.width()
                       && imageSize.height() <= canvasSize.height());

    // 能同时容纳"原画布"与"导入图"的最小尺寸（逐维取大）。
    //   fits 时它恰好等于原画布；
    //   不 fits 时它就是"扩大画布"该扩到的尺寸。
    //
    // ★ 走 engine/Geometry 的共用函数，不在这里重写一次 qMax ★
    //   这个尺寸同时被"对话框里告诉用户会扩到多大"和"真正扩画布"
    //   两处用到，必须同源 —— 写成两份策略的话，改了一处就会让
    //   提示的数字与最后的结果对不上，而这正是用户最容易发现、
    //   也最伤信任的那类不一致。
    //   理由与 canvasResizeOffset 完全一样（见那个函数的说明）。
    const QSize unionCanvas = unionSize(canvasSize, imageSize);

    bool growCanvas = false;

    if (!fits) {
        if (policy == GrowPolicy::Ask) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Question);
            box.setWindowTitle(tr("Image Is Larger Than the Canvas"));
            box.setText(tr("The imported image (%1 × %2) is larger than the current canvas (%3 × %4).")
                            .arg(imageSize.width()).arg(imageSize.height())
                            .arg(canvasSize.width()).arg(canvasSize.height()));
            box.setInformativeText(
                tr("'Grow Canvas to Fit' grows the canvas to %1 × %2, "
                   "centering both the existing content and the new "
                   "image;\n"
                   "'Crop to Canvas' keeps the canvas size, centers "
                   "the new image, and crops whatever sticks out.")
                    .arg(unionCanvas.width()).arg(unionCanvas.height()));

            QPushButton *growBtn   = box.addButton(tr("Grow Canvas to Fit"), QMessageBox::AcceptRole);
            QPushButton *cropBtn   = box.addButton(tr("Crop to Canvas"),     QMessageBox::DestructiveRole);
            QPushButton *cancelBtn = box.addButton(tr("Cancel"),           QMessageBox::RejectRole);
            box.setDefaultButton(growBtn);
            box.exec();

            // clickedButton() 在用户按 Esc 关掉时是 nullptr —— 必须一起挡掉，
            // 否则下面两次比较都为假，会走进"裁剪"分支（Esc 反而执行了操作）。
            if (box.clickedButton() == cancelBtn || box.clickedButton() == nullptr)
                return false;

            growCanvas = (box.clickedButton() == growBtn);
            Q_UNUSED(cropBtn);
        } else {
            // ★ 自动化：调用方已经替用户回答了这个问题 ★
            //   注意这里**没有**"默认裁剪"的兜底分支 —— GrowPolicy 只有
            //   三个值，Ask 已在上面处理掉，剩下的两个各自明确，
            //   不存在"什么都没选"的状态。
            growCanvas = (policy == GrowPolicy::Grow);
        }
    }

    // ── 要扩大画布时，先检查图层锁定 ──
    //
    // ★ 画布尺寸变更会**改写每一个图层**（逐图层重排到新尺寸），
    //   所以锁定的图层必须在这里被保护 —— 与 openCanvasSize 的检查同款。
    //   "加一层"本身不需要这个检查（它不碰现有图层），
    //   但"扩大画布"需要。
    if (growCanvas) {
        for (int i = 0; i < m_doc->layers().count(); ++i) {
            const Layer *l = m_doc->layers().layer(i);
            if (l && l->isLocked()) {
                if (m_statusBar)
                    m_statusBar->showMessage(
                        tr("Layer \"%1\" is locked; the canvas cannot grow. Unlock it before importing.")
                            .arg(l->name()), 4000);
                return false;
            }
        }
    }

    // ── 放置位置 ──
    //
    //  ★ 一个公式覆盖两种情况 ★
    //    在（可能已扩大的）画布里居中，**不**去分"放得下 / 放不下"两支。
    //    因为 targetCanvas 是逐维最大值，(targetCanvas - imageSize)
    //    必然非负，公式自动退化成两种情况：
    //      · 放得下 → 在原画布中居中（targetCanvas == canvasSize）
    //      · 放不下但选了扩大 → 在新画布中居中（此时至少有一个差值为 0）
    //    写成两支不仅更长，而且"放不下就贴左上角"这个常见写法会让用户
    //    看不到导入图的主体 —— 它的中心落在画布外，屏幕上只剩一个角。
    const QSize  targetCanvas = growCanvas ? unionCanvas : canvasSize;
    const QPoint placePos = centeredOffset(targetCanvas, imageSize);

    // ── 执行：扩画布（可选）+ 加图层，合成一条撤销 ──
    //
    // ★ 顺序不能反 ★
    //   GeometryChangeCommand 的前后快照是**整份图层列表 + 画布尺寸**。
    //   若先加图层再扩画布，快照里就会带上那个新图层，
    //   撤销"扩画布"时会连图层一起动 —— 两层命令的职责就串了。
    //   先扩画布（此时图层列表还是原样），再压 LayerCommand，职责清晰。
    m_commandStack->beginMacro(tr("Open as Layer"));

    if (growCanvas) {
        const GeometryChangeCommand::Snapshot before =
            GeometryChangeCommand::captureState(m_doc);

        // 走 Document::resizeCanvas + commitGeometryChange（几何变更的唯一入口）。
        //
        // ★ 绝不在这里自己 new GeometryChangeCommand ★
        //   那样会绕过 commitGeometryChange 里的**内存预检**
        //   （整层快照在 4 图层 × 4000×3000 下就是 384MB 一步），
        //   见 MainWindow.h 对该函数的说明与实现铁律 5.6。
        if (!m_doc->resizeCanvas(unionCanvas, Anchor::Center,
                                 Qt::transparent, /*smart=*/true)) {
            // ★ 失败路径必须把宏关掉 ★
            //   beginMacro 与 endMacro 不配对会让撤销栈永远停在"宏内"状态，
            //   接下来所有操作的命令都会被吞进这个宏里 —— 症状是
            //   "按一次 Ctrl+Z 把好几步一起撤销了"，非常难查。
            m_commandStack->endMacro();

            if (m_statusBar)
                m_statusBar->showMessage(tr("Failed to grow the canvas - import cancelled"), 3000);
            return false;
        }

        commitGeometryChange(before, GeometryChangeCommand::Kind::ResizeCanvas,
                             tr("Grow Canvas"));
    }

    // 图层名用文件名 —— 图层面板里显示 "photo.jpg" 比 "粘贴的图层" 有用得多
    addImageAsNewLayer(image, placePos, QFileInfo(path).fileName(),
                       tr("New Layer"));

    m_commandStack->endMacro();

    updateWindowTitle();

    if (m_statusBar) {
        m_statusBar->showMessage(
            growCanvas
                ? tr("Imported %1 (%2 × %3); the canvas grew to %4 × %5")
                      .arg(QFileInfo(path).fileName())
                      .arg(imageSize.width()).arg(imageSize.height())
                      .arg(m_doc->canvasSize().width())
                      .arg(m_doc->canvasSize().height())
                : tr("Imported %1 (%2 × %3) as a new layer")
                      .arg(QFileInfo(path).fileName())
                      .arg(imageSize.width()).arg(imageSize.height()),
            4000);
    }

    return true;
}

bool MainWindow::saveDocument()
{
    if (!requireDocument())
        return false;

    // 未命名文档（没有文件路径）→ 走"另存为"
    if (m_doc->filePath().isEmpty())
        return saveDocumentAs();

    // ── 保存到已有路径 ──
    //
    // ★ 格式与质量从原文件推断 ★
    //   用原扩展名决定格式，用默认质量。
    //   更精细的做法是在文档里记住"上次保存时用的质量"，
    //   这样用户设了 JPEG 质量 95 后，再次 Ctrl+S 仍然是 95。
    //   Phase 7 会在文档里加这两个字段。
    const QString path = m_doc->filePath();
    const QString suffix = QFileInfo(path).suffix().toLower();

    QByteArray format = suffix.toLatin1();
    if (format.isEmpty())
        format = "png";

    const int quality = (format == "jpg" || format == "jpeg")
                            ? kDefaultJpegQuality
                            : kDefaultPngQuality;

    // 是否包含 alpha：由格式能力决定。
    // ★ 不能一律传 true ★
    //   JPEG 不支持 alpha，写 PNG 之外的内容时传 true 在某些 Qt 版本上
    //   会导致保存失败或静默丢弃透明通道。
    const bool includeAlpha = formatSupportsAlpha(format);

    if (!writeImage(path, QLatin1String(format), quality, includeAlpha)) {
        // writeImage 内部已经弹了详细的错误对话框
        return false;
    }

    // ── 保存成功后的状态更新 ──
    if (m_commandStack)
        m_commandStack->setClean();   // 撤销栈标记为干净
    m_doc->setModified(false);
    updateWindowTitle();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Saved to %1").arg(QFileInfo(path).fileName()), 3000);

    return true;
}

bool MainWindow::saveDocumentAs()
{
    if (!requireDocument())
        return false;

    QSettings settings;
    const QString lastDir = settings.value(
        QStringLiteral("files/lastSaveDir"),
        m_doc->filePath().isEmpty() ? QStandardPaths::writableLocation(
                                          QStandardPaths::PicturesLocation)
                                    : QFileInfo(m_doc->filePath()).absolutePath())
                                .toString();

    // 默认文件名：沿用原名，或"未命名"
    const QString defaultName = m_doc->filePath().isEmpty()
                                    ? QStringLiteral("未命名.png")
                                    : QFileInfo(m_doc->filePath()).fileName();

    // ★ 用 QImageWriter 支持的格式动态生成过滤器 ★
    //   写死列表的话，用户系统上装的额外格式（如 TGA、PPM）就无法选择保存。
    const QStringList filters = writableFormatFilters();

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save As"), QDir(lastDir).filePath(defaultName),
        filters.join(QStringLiteral(";;")), &selectedFilter);

    if (path.isEmpty())
        return false;

    // ── 处理扩展名 ──
    //
    // ★ 这里必须处理两种情况 ★
    //   ① 用户输入了扩展名 → 用它的格式
    //   ② 用户没输扩展名 → 从选中的过滤器里取格式，并给路径补上扩展名
    //
    //   不做 ② 的话，用户输入 "output" 会写到一个没有扩展名的文件，
    //   下次打开时无法从扩展名识别格式（很多程序打不开它）。
    QString format = QFileInfo(path).suffix().toLower();

    if (format.isEmpty()) {
        // 从选中的过滤器里提取扩展名，如 "PNG 图像 (*.png)" → "png"
        if (!selectedFilter.isEmpty()) {
            const int starPos = selectedFilter.indexOf(QStringLiteral("*."));
            if (starPos >= 0) {
                QString ext = selectedFilter.mid(starPos + 2);
                // 过滤器里可能是 "*.jpg *.jpeg" 这种多扩展名形式
                const int spacePos = ext.indexOf(QLatin1Char(' '));
                if (spacePos > 0)
                    ext = ext.left(spacePos);
                ext = ext.remove(QLatin1Char(')')).trimmed();
                if (!ext.isEmpty()) {
                    format = ext.toLower();
                    path += QLatin1Char('.') + format;
                }
            }
        }
        if (format.isEmpty()) {
            // 兜底：默认 PNG
            format = QStringLiteral("png");
            path += QStringLiteral(".png");
        }
    }

    // ── 格式能力提示 ──
    if (!formatSupportsAlpha(format.toLatin1())) {
        // 目标格式不支持透明。如果图像真的用到了透明，必须提前告知 ——
        // 否则用户保存后发现"透明背景变成了黑色"，会以为程序有 bug。
        bool hasTransparency = false;
        if (m_doc->layerCount() > 0) {
            // 检查合成结果里是否有半透明像素。
            // ★ 不遍历全图，而是抽查 ★
            //   全图遍历 1200 万像素要 20ms，而抽查 1000 个点只要几微秒，
            //   且对"是否有任何透明像素"这个布尔问题已经足够可靠。
            const QImage comp = m_doc->composite();
            if (!comp.isNull()) {
                const int stepX = qMax(1, comp.width() / 32);
                const int stepY = qMax(1, comp.height() / 32);
                for (int y = 0; y < comp.height() && !hasTransparency; y += stepY) {
                    for (int x = 0; x < comp.width(); x += stepX) {
                        if (comp.pixelColor(x, y).alpha() < 255) {
                            hasTransparency = true;
                            break;
                        }
                    }
                }
            }
        }

        if (hasTransparency) {
            const QMessageBox::StandardButton answer = QMessageBox::warning(
                this, tr("Format Does Not Support Transparency"),
                tr("The %1 format does not support a transparency "
                   "channel.\n\n"
                   "Semi-transparent areas will be filled with "
                   "white.\n\n"
                   "Save as PNG instead to keep transparency.\n\n"
                   "Save anyway?").arg(format.toUpper()),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

            if (answer != QMessageBox::Yes)
                return false;
        }
    }

    const int quality = (format == QStringLiteral("jpg") || format == QStringLiteral("jpeg"))
                            ? kDefaultJpegQuality
                            : kDefaultPngQuality;

    if (!writeImage(path, format, quality, formatSupportsAlpha(format.toLatin1())))
        return false;

    // ── 更新文档的文件路径 ──
    //
    // ★ 另存为会改变文档的"归属文件" ★
    //   之后 Ctrl+S 应该保存到新路径，标题也应该显示新文件名。
    //   这一点容易漏掉 —— 漏掉的话用户另存为之后按 Ctrl+S，
    //   发现改动被写回了**原来**的文件，覆盖掉了原始文件。
    m_doc->setFilePath(path);

    if (m_commandStack)
        m_commandStack->setClean();
    m_doc->setModified(false);
    updateWindowTitle();

    settings.setValue(QStringLiteral("files/lastSaveDir"), QFileInfo(path).absolutePath());
    addRecentFile(path);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Saved as %1").arg(QFileInfo(path).fileName()), 3000);

    return true;
}

// ============================================================================
//  writeImage —— 实际的写文件
// ============================================================================
//
//  ★ 为什么用 QImageWriter 而不是 QImage::save() ★
//
//    QImage::save(path, format, quality) 只支持"质量"这一个参数，
//    而实际需要控制的还有：
//      · PNG 的压缩级别（影响文件大小与保存耗时）
//      · 是否写入 DPI 元数据（打印尺寸依赖它）
//      · 是否启用渐进式 JPEG（网传友好）
//      · 是否保留 EXIF
//
//    QImageWriter 提供了 setQuality / setCompression / setText 等接口，
//    还能通过 setOptimizedWrite / setProgressiveScanWrite 控制编码策略。
//
//    另一个重要原因：QImageWriter::errorString() 能给出**具体的**失败原因
//    （"权限不足" vs "磁盘已满" vs "格式不支持"），
//    而 QImage::save 只返回一个 bool。用户看到"保存失败"时
//    完全不知道该怎么办。
// ===========================================================================
//  导出为 ICO 图标
// ===========================================================================
//
//  ★★ 为什么它必须是一个**公开、无对话框**的入口 ★★
//
//    另存为那条路的第一件事是 QFileDialog —— 模态对话框在无人的环境里
//    会把进程**永久挂住**（不是报错，是整个测试/截图流程停在那儿）。
//    所以"选路径"和"真正干活"必须拆开：本函数只管"给我路径，我把它写成
//    ICO"，对话框留在调用方（writeImage 那条 UI 路径）。
//    这样自检与截图工具都能直接驱动它，512 上限也就有断言守着了。
//
//    与 importImageAsLayer()（打开为图层）是同一个套路。
//
//  ★ 返回值 ★
//    true  = 文件已写出
//    false = 尺寸超限 / 无文档 / 写文件失败，原因写进 errorOut（可为 nullptr）
//
//  ★ 本函数**不弹任何对话框**，失败信息全部走 errorOut ★
//    弹窗由 writeImage 负责 —— 它是 UI 路径，那里才确定"有人可问"。
bool MainWindow::exportIcon(const QString &path, QString *errorOut)
{
    if (!m_doc) {
        if (errorOut)
            *errorOut = tr("No image is open.");
        return false;
    }

    const QSize size = m_doc->canvasSize();

    // ── ★★ 尺寸预检：必须在**合成之前** ★★ ──
    //
    //  "太大了"这个答案只依赖画布尺寸，不依赖任何像素。
    //  先合成再拒绝 = 白白合成一张可能上千万像素的大图，
    //  而它马上就会被丢掉（用户看到的只是"不行"）。
    if (!ico::canEncode(size)) {
        if (errorOut)
            *errorOut = ico::rejectionMessage(size);
        return false;
    }

    // ── 取合成结果（与另存为同一条路：存的是"看到的画面"）──
    QImage image = m_doc->composite();
    if (image.isNull()) {
        if (errorOut)
            *errorOut = tr("Cannot generate the image data (the composite is empty).");
        return false;
    }

    // ── 编码 + 写文件 ──
    //
    //  ★ 这里**不做** 预乘 → 非预乘 的转换 ★
    //    那是编码器的内部职责（ico::encode 自己会转）：
    //    转换规则属于"文件格式要求"，散到调用方就会出现
    //    "这条路径转了、那条路径忘了"—— 而忘掉的那个只在
    //    半透明边缘上出问题，最难发现。只有一份实现，就不会漏。
    return ico::write(path, image, errorOut);
}

// ===========================================================================
//  打印 / PDF
// ===========================================================================

namespace {

// 按图像的物理尺寸算纸张（毫米），并夹在 50mm ~ A0 之间。
//
// 夹紧的两头都不是理论问题：16×16 的图标按 72dpi 印出来只有 5.6mm，
// 落在纸上是个看不清的小点；而 20000px 的长图会要求打印机提供一张
// 七八米长的纸 —— 打印对话框会直接拒绝这个尺寸。
QSizeF paperSizeMm(const QImage &image, qreal dpi)
{
    const qreal safeDpi = (dpi > 1.0) ? dpi : 72.0;
    const qreal w = image.width() / safeDpi * 25.4;
    const qreal h = image.height() / safeDpi * 25.4;
    return QSizeF(qBound(50.0, w, 841.0), qBound(50.0, h, 1189.0));
}

}   // namespace

bool MainWindow::renderToPrinter(QPrinter *printer)
{
    if (!printer || !m_doc)
        return false;

    const QImage image = m_doc->composite();
    if (image.isNull())
        return false;

    QPainter painter;
    if (!painter.begin(printer))
        return false;

    // 可打印区域（设备像素）。不用整页尺寸，否则打印机的物理页边距
    // 会盖住图的边缘。
    const QRectF page = printer->pageRect(QPrinter::DevicePixel);

    // 等比缩放 + 居中。宁可留白，也不要把图拉变形 ——
    // 一张被拉伸过的图比一张小的图更难接受。
    const qreal scale = qMin(page.width() / image.width(),
                             page.height() / image.height());
    QRectF target(0, 0, image.width() * scale, image.height() * scale);
    target.moveCenter(page.center());

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image);
    painter.end();
    return true;
}

bool MainWindow::exportPdf(const QString &path, QString *errorOut)
{
    if (!m_doc) {
        if (errorOut)
            *errorOut = tr("No image is open.");
        return false;
    }

    const QImage image = m_doc->composite();
    if (image.isNull()) {
        if (errorOut)
            *errorOut = tr("Cannot generate the image data (the composite is empty).");
        return false;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setDocName(m_doc->displayName());

    // 纸张照图像的物理尺寸定；PDF 没有物理页边距的限制，所以边距给 0，
    // 让图正好铺满一页（读者缩放时不会看到一圈莫名留白）。
    const QSizeF paper = paperSizeMm(image, m_doc->dpiX());
    printer.setPageSize(QPageSize(paper, QPageSize::Millimeter));
    printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);

    if (!renderToPrinter(&printer)) {
        if (errorOut)
            *errorOut = tr("Failed to write the PDF file.");
        return false;
    }
    return true;
}

void MainWindow::printDocument()
{
    if (!requireDocument())
        return;

    // 低 glibc 的 Qt5 AppImage 刻意不打包 libcups（否则会把 glibc 2.27 的库链
    // 拖进去，见 release.yml），该环境下没有 CUPS 打印后端。
    // availablePrinters 为空时提前说明并指向导出 PDF —— 不检测的话，
    // 用户只会看到一个空打印机列表的对话框，不知道发生了什么。
    if (QPrinterInfo::availablePrinters().isEmpty()) {
        if (m_statusBar) {
            m_statusBar->showMessage(
                tr("Printing is unavailable in this build; use Export PDF instead"),
                8000);
        }
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(m_doc->displayName());

    // 给一个合理的初始纸张，用户仍可在对话框里改。
    const QImage image = m_doc->composite();
    if (!image.isNull())
        printer.setPageSize(QPageSize(paperSizeMm(image, m_doc->dpiX()),
                                      QPageSize::Millimeter));

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print"));
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (!renderToPrinter(&printer)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Printing failed"), 5000);
        return;
    }

    if (m_statusBar)
        m_statusBar->showMessage(tr("Sent to printer"), 4000);
}

void MainWindow::saveAsPdf()
{
    if (!requireDocument())
        return;

    QString suggested;
    if (m_doc->filePath().isEmpty()) {
        suggested = QDir::homePath() + QLatin1Char('/')
                    + tr("Untitled") + QStringLiteral(".pdf");
    } else {
        const QFileInfo fi(m_doc->filePath());
        suggested = fi.absolutePath() + QLatin1Char('/')
                    + fi.completeBaseName() + QStringLiteral(".pdf");
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export PDF"), suggested, tr("PDF files (*.pdf)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!exportPdf(path, &error)) {
        QMessageBox::warning(this, tr("Export PDF"), error);
        return;
    }

    if (m_statusBar)
        m_statusBar->showMessage(
            tr("PDF exported: %1").arg(QFileInfo(path).fileName()), 5000);
}

// ===========================================================================
//  写图像文件
// ===========================================================================

bool MainWindow::writeImage(const QString &path, const QString &format,
                            int quality, bool includeAlpha)
{
    if (!m_doc)
        return false;

    // ── ★★ ICO 不在 QImageWriter 这条路上 ★★ ──
    //
    //  ★ 为什么必须在这里分流，而不是让 QImageWriter 去写 ★
    //
    //    ① **512 上限没有地方可拦**。QImageWriter 会闷头往下写；
    //       老版本 Qt 的 ICO 插件干脆把超过 256 的图**静默缩到 256** ——
    //       用户以为存下了 512 的图标，其实拿到一张 256 的，
    //       而且程序一声不响。这是"看起来成功了"的失败。
    //    ② **qico.dll 是要单独部署的插件**（imageformats 目录）。
    //       漏打一个 dll 的症状是"PNG 能存、ICO 存不了"，
    //       而且要到运行期才发现。自写编码器没有这个依赖。
    //    ③ 自写的那份能被离线断言钉住字节结构（见 tests/ico_test.cpp）。
    //
    //  ★ 分流点只此一处 ★
    //    saveDocument()（Ctrl+S 到已有 .ico）与 saveDocumentAs()（另存为）
    //    都经过 writeImage，所以在这里分流 = 两条路自动都被覆盖，
    //    不可能出现"另存为能存 ICO、Ctrl+S 又走回插件"这种不一致。
    if (format.compare(QStringLiteral("ico"), Qt::CaseInsensitive) == 0) {
        QString err;
        if (exportIcon(path, &err))
            return true;

        // ★ 尺寸超限是"用户必须知道原因"的失败 ★
        //   只说"保存失败"的话，用户会反复重试、或者以为程序坏了。
        QMessageBox::warning(this,
                             tr("Cannot Save as ICO"),
                             err.isEmpty() ? tr("Failed to export the ICO icon.") : err);
        return false;
    }

    // ── 取合成结果 ──
    //
    // ★ 保存的是合成结果，不是活动图层 ★
    //   用户看到的画面是合成后的，保存自然也应该保存"看到的"。
    //   保存活动图层会导致"图层面板里明明有三层，保存出来的只有一层"。
    QImage image = m_doc->composite();

    if (image.isNull()) {
        QMessageBox::warning(this, tr("Save Failed"),
                             tr("Cannot generate the image data (the composite is empty)."));
        return false;
    }

    // ── 格式转换 ──
    //
    // ★ 关键：预乘格式不能直接写文件 ★
    //
    //   本工程内部统一使用 ARGB32_Premultiplied。但绝大多数图像格式
    //   （PNG / JPEG / BMP）存的是**非预乘**的值。
    //   如果直接把预乘数据写出去，读取时会被当作非预乘数据解释 ——
    //   表现为"半透明区域的颜5色明显偏暗"（因为 RGB 已经被乘过 alpha 了）。
    //
    //   这个 bug 特别隐蔽：不透明的像素完全正常（alpha=255 时预乘无影响），
    //   只有带透明度的边缘会出错。而恰好是"带透明边缘的图"才会被发现。
    if (image.format() == QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }

    // 不支持 alpha 的格式：先转换到 RGB32。
    // ★ 转换前必须先做 alpha 合成 ★
    //   convertToFormat(Format_RGB32) 会**丢弃** alpha 通道，
    //   而预乘数据被丢 alpha 后颜色会变暗（相当于"把半透明当不透明"）。
    //   正确做法是先合成到白色背景上。
    if (!includeAlpha && image.hasAlphaChannel()) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.fill(Qt::white);
        QPainter p(&flattened);
        p.drawImage(0, 0, image);
        p.end();
        image = flattened;
    }

    // ── 写入 DPI 元数据 ──
    //
    //  不写的话，保存出来的图在 Word/Illustrator 里插入时会被当作 72 DPI，
    //  打印尺寸与预期不符（300 DPI 的图会被放大 4 倍多）。
    //  QImageWriter 会从 QImage 的 dotsPerMeterX/Y 自动读取，
    //  所以这里只需要把 DPI 写进 QImage。
    writeDpiToImage(image, m_doc->dpiX(), m_doc->dpiY());

    // ── 编码 ──
    QImageWriter writer(path, format.toLatin1());

    if (formatSupportsQuality(format.toLatin1()))
        writer.setQuality(quality);

    // PNG 的压缩级别用 quality 控制。
    // Qt 的 PNG 插件把 setQuality 解释为"压缩强度"（0-100），
    // 与 JPEG 的质量语义相反（JPEG 是越高越好，PNG 是越高越小但越慢）。
    // 这里对 PNG 单独设一个合理的值。
    if (format.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0)
        writer.setQuality(kDefaultPngQuality);

    // 渐进式 JPEG：让网传时能先看到模糊轮廓。
    // 代价是文件略大（约 5%）。对本地编辑程序，这个收益不大，
    // 但显式关闭能避免不同 Qt 版本默认值不一致导致的文件大小波动。
    writer.setProgressiveScanWrite(false);

    if (!writer.write(image)) {
        QMessageBox::warning(this, tr("Save Failed"),
            tr("Cannot write the file:\n%1\n\n"
               "Reason: %2\n\n"
               "Please check:\n"
               "  · whether the target directory exists and is writable\n"
               "  · whether there is enough disk space\n"
               "  · whether the file is occupied by another program")
                .arg(path)
                .arg(writer.errorString()));
        return false;
    }

    return true;
}

void MainWindow::closeCurrentDocument()
{
    // 菜单「文件 → 关闭」与标签上的 × 走**同一条**路径。
    // ★ 这是"单一实现"的又一处 ★
    //   两条入口各写一遍的话，"点 × 会问保存、菜单不会"这类不一致
    //   只在用户恰好用另一个入口时才暴露。
    if (m_tabs)
        closeDocumentTab(m_tabs->currentIndex());
}

// ============================================================================
//  closeDocumentTab —— 关闭一页
// ============================================================================

void MainWindow::closeDocumentTab(int index)
{
    if (!m_tabs || index < 0 || index >= m_tabs->count())
        return;

    // ── ① 先把这一页切到前面 ──
    //
    //  ★ 为什么必须切 ★
    //    未保存确认与保存都作用在**当前页**上（maybeSave / saveDocument /
    //    requireDocument 全部以 m_doc 为准）。为"关闭后台标签"再写一套
    //    吃 Document* 的重载，等于把这几百行整体复制一遍 ——
    //    而复制品没有自检守着，改原版的人也不会记得同步它。
    //    用户点了某一页的 ×，把它切出来本来也是最自然的反馈。
    //
    //  ★ 为什么连 activateTab 都要显式调 ★
    //    DocumentTabs 在摘页/增页期间屏蔽了自己的信号（见那边的说明），
    //    所以上一步的 setCurrentIndex() 不会发 currentChanged。
    //    依赖信号 = 切页永远不会发生 = 下面 maybeSave() 问的是别的一页。
    if (m_tabs->currentIndex() != index) {
        {
            const QScopedValueRollback<bool> guard(m_switchingTabs, true);
            m_tabs->setCurrentIndex(index);
        }
        activateTab(index);
    }

    // ── ② 未保存确认（针对这一页的文档）──
    if (!maybeSave())
        return;                 // 用户取消 → 页面原样留着，什么都不做

    // ── ③ 工具收尾 ──
    //   画笔正在画一笔时用户点了 ×，这一笔的内容已经在图层里了，
    //   不 commit 就没有对应的撤销记录 —— 而且"未保存提示"也可能
    //   因为栈还是 clean 而不出现，结果用户丢了一笔却毫无提示。
    if (m_toolManager)
        m_toolManager->commitActiveTool();

    Document *doc = m_doc;
    const int docIndex = doc ? m_docManager->indexOf(doc) : -1;
    if (!doc || docIndex < 0)
        return;

    // ── ④ 摘页 → 重绑 → 销毁文档，中间屏蔽同步 ──
    //
    //  ★ 这三步的顺序是有讲究的，不能换 ★
    //
    //     ㈠ 摘页：这一页的画布与撤销栈一起销毁。此时**所有文档都还活着**。
    //     ㈡ 重绑：把界面切到剩下的某一页上。必须在删文档**之前**做 ——
    //        否则中间会有一段"工具、面板、状态栏都还指着那份已经被
    //        delete 的文档"的窗口期，而任何一次重绘或析构都可能碰到它。
    //        （这个窗口期在旧代码里靠"先 closeDocument 再等信号"侥幸避开了，
    //          因为那时信号是同步发的；现在没有那个信号了，必须显式做。）
    //     ㈢ 删文档：最后才销毁，此时已经没有任何部件引用它。
    //
    //  ★ 中间必须屏蔽同步 ★
    //    摘页与删文档各自都会让"当前是哪一个"变化，而这两个中间态里
    //    "标签序号"与"文档序号"并不一致（一个动过了、另一个还没动）。
    //    让外面看到它，同步逻辑就会把当前文档指到错误的那一个上 ——
    //    而且不报任何错。
    {
        const QScopedValueRollback<bool> guard(m_switchingTabs, true);

        m_tabs->removeDocumentTab(index);          // ㈠ 画布 + 撤销栈随页销毁
        activateTab(m_tabs->currentIndex());       // ㈡ 先把界面切到还活着的文档
        m_docManager->closeDocument(docIndex);     // ㈢ 立刻 delete 文档
    }

    // ── ⑤ 显式补推当前状态 ──
    //    屏蔽期间信号被丢掉了，必须手动走一遍（理由见第 ④ 步）。
    //    传 m_tabs->currentIndex()：关掉最后一页时它是 -1，
    //    activateTab(-1) 会把界面带进"空状态"（所有文档相关菜单变灰）。
    activateTab(m_tabs->currentIndex());

    if (m_statusBar) {
        m_statusBar->showMessage(
            m_tabs->count() == 0
                ? tr("Document Closed")
                : tr("Closed a document (%1 left)").arg(m_tabs->count()),
            3000);
    }
}

// ============================================================================
//  maybeSave —— 未保存提示
// ============================================================================
//
//  ★ 返回值语义 ★
//    true  —— "可以继续"（用户选择了保存并成功，或选择了不保存）
//    false —— "取消操作"（用户点了取消，或保存失败）
//
//    所有"会丢弃当前页"的操作（关闭标签、关闭窗口）都必须在
//    第一步调用它并检查返回值。
//
//  ★ 为什么"新建/打开"不在这个列表里了 ★
//    它们不再丢弃任何东西（新开一页而已），所以不需要问。
//    这也让"确认"这件事只剩两个入口，审查起来清楚得多。
//
//  ★ 为什么要用三按钮而不是二按钮 ★
//    只有"是/否"的话，用户想反悔（点错了关闭）就没有退路 ——
//    只能先选"否"（丢弃修改）才能继续，而修改已经丢了。
//    "是/否/取消"是标准做法。
bool MainWindow::maybeSave()
{
    // 没有任何文档，或当前文档没有修改 → 直接放行
    if (!needsSave(m_doc, m_commandStack))
        return true;

    const QString name = m_doc->filePath().isEmpty()
                             ? m_doc->displayName()
                             : QFileInfo(m_doc->filePath()).fileName();

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Unsaved Changes"),
        tr("\"%1\" has unsaved changes.\n\nSave them?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    switch (answer) {
    case QMessageBox::Save:
        // 保存成功 → 继续；保存失败（用户取消了另存为对话框）→ 中止
        return saveDocument();

    case QMessageBox::Discard:
        return true;

    case QMessageBox::Cancel:
    default:
        return false;
    }
}

// ============================================================================
//  maybeSaveAll —— 逐页问过去（关窗口用）
// ============================================================================
//
//  ★ 为什么必须逐页问 ★
//    只问当前页是不够的：另外三页里可能有一页改了没保存，
//    用户点"关闭"之后它就随着进程一起没了，而且**没有任何提示**。
//    单文档时代这个问题不存在（只有一个文档），多标签一来它就是个
//    真实的数据丢失路径。
//
//  ★ 为什么"有修改的那一页先切到前面再问" ★
//    这样就能直接复用"当前页"那一套问答与保存逻辑（maybeSave /
//    saveDocument / 另存为对话框），而不必再写一份吃 Document* 的重载。
//    顺带还有个好处：弹"是否保存「photo.jpg」"时，用户看到的正是
//    那一页的内容 —— 而不是被问到一张看不见的图。

bool MainWindow::maybeSaveAll()
{
    if (!m_tabs)
        return true;

    for (int i = 0; i < m_tabs->count(); ++i) {
        const DocumentTab *tab = m_tabs->tabAt(i);
        if (!tab || !tab->doc)
            continue;

        // ★ 判据与 maybeSave() 完全一致 ★
        //   两处用不同的判据会出现"标签上有 *，但关闭时不问"。
        //   见文件头 needsSave() 的说明。
        if (!needsSave(tab->doc, tab->stack))
            continue;

        if (m_tabs->currentIndex() != i) {
            {
                const QScopedValueRollback<bool> guard(m_switchingTabs, true);
                m_tabs->setCurrentIndex(i);
            }
            activateTab(i);
        }

        if (!maybeSave())
            return false;       // 任何一页被取消 → 整体取消
    }

    return true;
}

// ============================================================================
//  eventFilter —— 接收系统的"用本程序打开文件"事件 / 画标尺拐角
// ============================================================================
//
//  macOS 上把文件拖到 Dock 图标、或在 Finder 里选"打开方式"，系统不会给
//  命令行参数，而是往应用里投一个 QEvent::FileOpen。这个事件发给
//  QApplication，所以在 qApp 上装了过滤器。
//
//  其它平台不会发这个事件，这个分支自然不成立 —— 不加平台条件，
//  少一处"换平台忘了改"。
//
//  ★ 另一个用途：标尺拐角 ★
//    它不是普通 QWidget —— 必须和标尺**同色**，而标尺的颜色来自
//    RulerWidget 自己的 palette。用子类或另存一份颜色都会让两处不同步，
//    所以把它的绘制也收在这里，颜色从**它旁边那把标尺**的 palette 取，
//    一处来源，派生关系与标尺完全一致。

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched && watched == m_rulerCorner && event->type() == QEvent::Paint) {
        auto *w = static_cast<QWidget *>(watched);
        QPainter p(w);
        // 用横向标尺当色源：它与拐角紧邻同一行，且两者 palette 同源。
        const QPalette pal = m_hruler ? m_hruler->palette() : w->palette();
        p.fillRect(w->rect(), pal.color(QPalette::Window));
        // 与标尺一致的分界线：右侧、下侧各一条（靠画布方向）
        p.setPen(pal.color(QPalette::Midlight));
        p.drawLine(w->width() - 1, 0, w->width() - 1, w->height() - 1);
        p.drawLine(0, w->height() - 1, w->width() - 1, w->height() - 1);
        return true;          // 自绘：不要再让 QWidget 去填背景
    }

    if (event->type() == QEvent::FileOpen) {
        auto *openEvent = static_cast<QFileOpenEvent *>(event);
        const QString path = openEvent->file();
        if (!path.isEmpty()) {
            openDocumentPaths(QStringList() << path);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ============================================================================
//  setActionIcon / refreshAllIcons / changeEvent —— 图标跟随主题
// ============================================================================

void MainWindow::setActionIcon(QAction *action, const QString &name)
{
    if (!action)
        return;
    action->setIcon(icons::icon(name));
    // ★ 名字记在动作上 ★
    //   光设图标的话，换主题之后没有任何线索能把这些动作"反查"回图标名 ——
    //   于是只能遍历全部动作猜。存一份名字是最省事也最不会漏的办法。
    action->setProperty("iconName", name);
}

// ----------------------------------------------------------------------------
//  setTheme —— 切换界面主题
// ----------------------------------------------------------------------------
//
//  ★ 为什么这里不调 refreshAllIcons ★
//    换 palette 会由 Qt 广播 QEvent::PaletteChange 到每个窗口，changeEvent
//    自己会走 refreshAllIcons —— 在这里再调一次是白刷两遍。
//    只有一种情况要补：目标主题与当前配色**恰好相同**（用户点了已经勾着的
//    那一项）。那时 palette 没变，不会来事件，但勾选态仍要同步回来。
void MainWindow::setTheme(uitheme::Mode mode)
{
    uitheme::setPreference(mode);     // 先落盘：用户可能点完就 Alt+F4
    uitheme::applyTheme(mode);

    // 勾选态无条件按"偏好"重来一遍。
    // ★ 不能只在变化时改 ★ QActionGroup 自己会把点击项设成 checked，
    //   但"自动"在深色系统下点一次会看不出勾到哪 —— 统一按偏好写，
    //   菜单与设置永远一致。
    for (QAction *act : m_actionTheme) {
        if (!act)
            continue;
        act->setChecked(act->data().toInt() == int(mode));
    }

    // 兜底：目标配色与当前相同时不会来 PaletteChange，图标要自己刷一次。
    // refreshAllIcons 内部先比颜色，没变就直接返回 —— 不会白刷。
    refreshAllIcons();
}

// ----------------------------------------------------------------------------
//  refreshAllIcons —— 按当前 palette 重取全部图标
// ----------------------------------------------------------------------------
//
//  调用时机只有两处：changeEvent() 收到 PaletteChange 且颜色真的变了；
//  setTheme()（同一主题重选时没有事件可等）。
//
//  ★ 为什么必须"重设"而不是只清缓存 ★
//    图标是渲染时刻染色的（见 IconLibrary.h 的说明）。清缓存只影响下次取图，
//    已经 setIcon 进菜单/工具栏的那张旧色图不会自己变 ——
//    用户看到的仍然是"切了深色，图标还是黑的"。
//
//  ★ 为什么先比颜色 ★
//    PaletteChange 在拖动窗口、切换焦点窗口时也会来（Qt 会把非活动窗口的
//    某些颜色改掉）。颜色没变就重设 74 个动作 × 5 档 × 2 倍率纯属白干。
void MainWindow::refreshAllIcons()
{
    const QColor color = icons::currentColor();
    if (color == m_lastIconColor)
        return;
    m_lastIconColor = color;

    icons::clearCache();      // ★ 必须先清 ★ 否则取回来的还是旧色的缓存项

    const QList<QAction *> all = findChildren<QAction *>();
    for (QAction *a : all) {
        const QString name = a->property("iconName").toString();
        if (name.isEmpty())
            continue;
        const QIcon ic = icons::icon(name);
        if (!ic.isNull())
            a->setIcon(ic);
    }

    // 工具按钮：图标来自工具 id（不是属性表），得单独走一遍。
    // ★ 必须整条降级链照 buildToolBox 重来 ★ 只换"专用图标"那一级的话：
    //   · 没有配图标的工具会从字母占位图变成空按钮（IconOnly 模式下是空白）；
    //   · 字母占位图自己是按旧 palette 画的，也是旧色。
    //   所以这里按工具名重画，而不是"能取到就设、取不到就跳过"。
    for (auto it = m_toolActions.constBegin(); it != m_toolActions.constEnd(); ++it) {
        QAction *a = it.value();
        if (!a)
            continue;
        const QString id = it.key();
        QString text = id;
        if (m_toolManager) {
            if (const AbstractTool *t = m_toolManager->findById(id))
                text = t->displayName();
        }
        QIcon ic = icons::iconForTool(id);
        if (ic.isNull())
            ic = makeLetterIcon(text);
        a->setIcon(ic);
    }

    if (m_layerPanel)
        m_layerPanel->refreshIcons();

    // 工具箱底部的"前景色"色块也是 palette 派生的
    updateColorIndicators();
}

// ----------------------------------------------------------------------------
//  changeEvent —— 主题切换
// ----------------------------------------------------------------------------
//
//  ★ 为什么用 PaletteChange 而不是 colorSchemeChanged ★
//    colorSchemeChanged 是 Qt 6.5 才有的信号，而本工程还要支持 Qt 5.15。
//    PaletteChange 两个版本都有，且它反映的正是"前景/底色真的换了"这件事 ——
//    对"图标该用什么颜色"来说，它比"系统设成了深色"更贴题
//    （应用被强制成浅色主题时，palette 就是浅的）。
void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        refreshAllIcons();
}

// ============================================================================
//  closeEvent —— 窗口关闭
// ============================================================================

void MainWindow::closeEvent(QCloseEvent *event)
{
    // ★ 先让工具收尾 ★
    //   画笔正在画一笔时用户点了关闭按钮（或按 Alt+F4），
    //   如果不 commit，这一笔的内容已经在图层里了但没有撤销记录，
    //   而且"未保存提示"也可能因为撤销栈还是 clean 而不出现 ——
    //   结果是用户丢掉了一笔却没有任何提示。
    if (m_toolManager)
        m_toolManager->commitActiveTool();

    // ★ 逐页问，任何一页被取消就整体取消 ★
    if (!maybeSaveAll()) {
        event->ignore();
        return;
    }

    // 保存设置（窗口几何、停靠状态、工具参数）
    saveSettings();

    // 正常退出：本实例的恢复快照没有留存价值。
    // 只删自己的 key——同目录可能还有另一个实例的（它用不同的 pid 前缀）。
    if (m_recoveryManager) {
        for (const QString &key : m_recoveryKeys)
            m_recoveryManager->discard(key);
        m_recoveryKeys.clear();
    }

    // ★ 顺序：先拆 UI 页 → 再让面板松手 → 最后销毁文档 ★
    //
    //  DocumentTabs 持有每页的撤销栈，而撤销栈里的命令持有 Document*。
    //  反过来（先销毁文档）会出现一个"栈还在、文档已经没了"的窗口期 ——
    //  那时任何一条析构路径碰到文档都是野指针。
    //  先 clearAllTabs()：画布与撤销栈随页一起干净地消失，
    //  之后再销毁文档就没有人指着它们了。
    if (m_tabs)
        m_tabs->clearAllTabs();

    // ★ 让界面进入"空状态"—— 这一步不能省 ★
    //
    //  clearAllTabs() 拆的只是**页**（画布与撤销栈随页消失），
    //  而面板（图层面板 / 状态栏 / 工具管理器）手里还攥着各页的
    //  Document* —— 它们要等 applyDocumentToPanels(nullptr) 才会松手。
    //
    //  不推这一次的话，下面 closeAll() 一执行，那些指针就指向被释放的
    //  内存了。而本窗口随后还要走一轮完整析构，子对象的析构路径里
    //  （面板换绑/断开时都会 disconnect(m_doc, …)、读 m_doc->xxx()）
    //  只要碰一下就是 use-after-free。
    //  实测症状：**关掉窗口之后再销毁窗口 → 段错误**。
    //  真实程序里进程紧接着就退出了，所以这个野指针一直没被人看见 ——
    //  它是一颗只在"关窗口之后还有代码跑"时才炸的雷
    //  （自动化测试正是那种代码）。
    //
    //  ★ 为什么走 activateTab(-1) 而不是手写三行 nullptr ★
    //    "当前页变化"全工程只有 activateTab 一个入口，**空状态也是它
    //    的一种**（传 -1）。手写三行只能把 MainWindow 自己的指针置空，
    //    面板那一侧照样攥着 —— 正是要修的东西漏掉了。
    //    closeDocumentTab() 关掉最后一页时走的也是这条路，两边一致。
    activateTab(-1);

    if (m_docManager)
        m_docManager->closeAll();

    event->accept();
}

// ============================================================================
//  dragEnterEvent / dropEvent —— 拖文件到窗口打开
// ============================================================================

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // ★ 只接受"本地文件"的拖放 ★
    //   从浏览器里拖一张图进来时，拖放数据是 URL（可能指向网络资源），
    //   下载它需要网络访问且可能很慢 —— 超出"轻量级编辑器"的职责范围。
    //   只处理 hasUrls + 本地文件，行为可预期。
    const QMimeData *mime = event->mimeData();

    if (!mime->hasUrls())
        return;

    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile())
            continue;

        const QString path = url.toLocalFile();

        // 用 QImageReader 判断"这个文件能不能被本程序解码"。
        // ★ 比只检查扩展名可靠 ★
        //   扩展名可以随意改（把 .txt 改成 .png），
        //   而 QImageReader::canRead() 会实际去探测文件头。
        if (QImageReader(path).canRead()) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime->hasUrls())
        return;

    // ── 收集全部可读的文件 ──
    //
    // ★ 多标签之后，拖进来的文件**全部打开** ★
    //   旧实现只开第一个（单文档装不下第二个），并给一句
    //   "当前版本一次只能打开一个"的提示 —— 那是个被架构限制逼出来的
    //   功能缺口。现在每个文件开一页，"拖 5 张图进来得到 5 个标签"
    //   才是符合直觉的行为。
    QStringList paths;
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile())
            continue;

        const QString path = url.toLocalFile();

        // 用 QImageReader 判断"这个文件能不能被本程序解码"。
        // ★ 比只检查扩展名可靠 ★
        //   扩展名可以随意改（把 .txt 改成 .png），
        //   而 QImageReader::canRead() 会实际去探测文件头。
        if (QImageReader(path).canRead())
            paths << path;
    }

    if (paths.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();

    // ★ 与命令行参数走同一条路 ★
    //   两者的语义完全相同（"给我一串路径，每个开一页"），
    //   各写一份的话"重复文件怎么办"这类规则必然只有一份会被更新到位。
    openDocumentPaths(paths);
}

// ============================================================================
//  keyPressEvent —— 单键工具快捷键与颜色快捷键
// ============================================================================
//
//  ★ 为什么不在 QAction 上设单键快捷键 ★
//
//    Qt 的 QAction 快捷键是"应用级"的 —— 只要焦点在窗口内就会触发，
//    无法排除"用户正在某个文本框里输入"的情况。
//
//    具体问题：用户在"透明度"输入框里想输入字母 x，
//    结果触发了"交换前景/背景色"，输入框里什么都没输进去。
//    这类问题非常烦人，而且用户很难猜到原因。
//
//  ★ 正确的做法 ★
//    在 MainWindow::keyPressEvent 里手动判断：
//      1. 事件是否已被处理（event->isAccepted()）
//      2. 焦点控件是否是输入类控件（QLineEdit / QAbstractSpinBox / QTextEdit…）
//      3. 才决定是否执行快捷键
//
//    ★ 派发顺序：先工具，后全局快捷键 ★
//      这样"移动工具的方向键"不会被全局的"滚动"抢走。
//      （方向键的滚动由 CanvasView 处理，它在更内层。）
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    // ── 忽略带修饰键的组合 ──
    //   那些由 QAction 的快捷键处理。
    //   只处理"裸键"（无 Ctrl/Alt/Meta）。
    //   ★ Shift 要放行 ★ 因为快捷键可能定义成 Shift+X 的变体，
    //   而且大写字母输入会产生 Shift 标志 —— 这里只关心字母本身。
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (mods.testFlag(Qt::ControlModifier)
        || mods.testFlag(Qt::AltModifier)
        || mods.testFlag(Qt::MetaModifier)) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    // ── 焦点在输入控件里？不处理 ──
    //
    // ★ 这是本函数存在的核心理由 ★
    //   用 QApplication::focusWidget() 拿到当前焦点控件，
    //   如果是"文本输入类"就完全不干预 ——
    //   用户输入的每一个字符都应该是给他的输入框的。
    if (QWidget *focus = QApplication::focusWidget()) {
        const QString className = QLatin1String(focus->metaObject()->className());

        // ★ 用类名判断而不是 qobject_cast ★
        //   原因：qobject_cast 需要 include 每种控件的头文件，
        //   而且无法覆盖用户自定义的输入控件（它们的类名通常包含
        //   "Edit"/"Spin"/"Line" 等词，用字符串匹配反而更全面）。
        //
        //   判断依据是"这个控件的类名里是否含输入控件的关键词"。
        const bool isInputWidget =
            className.contains(QStringLiteral("QLineEdit"))
            || className.contains(QStringLiteral("QTextEdit"))
            || className.contains(QStringLiteral("QPlainTextEdit"))
            || className.contains(QStringLiteral("QAbstractSpinBox"))
            || className.contains(QStringLiteral("QSpinBox"))
            || className.contains(QStringLiteral("QDoubleSpinBox"))
            || className.contains(QStringLiteral("QComboBox"))
            || className.contains(QStringLiteral("QFontComboBox"))
            || className.contains(QStringLiteral("QListView"))
            || className.contains(QStringLiteral("QTreeView"))
            || className.contains(QStringLiteral("QTableView"));

        if (isInputWidget) {
            QMainWindow::keyPressEvent(event);
            return;
        }
    }

    // ── 颜色快捷键 ──
    if (event->key() == Qt::Key_X) {
        swapColors();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_D) {
        resetColors();
        event->accept();
        return;
    }

    // ── 工具快捷键 ──
    //
    //  用事件里的 key() 去 ToolManager 查。
    //
    //  ★ 为什么不用 QShortcut ★
    //    工具快捷键需要"工具优先于菜单"的派发顺序，
    //    而 QShortcut 的派发顺序在 Qt 内部固定，不好控制。
    //    手动在 keyPressEvent 里查最简单可靠。
    if (activateToolByShortcut(event->key())) {
        event->accept();
        return;
    }

    // ── 其余交给父类 ──
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::activateToolByShortcut(int key)
{
    if (!m_toolManager)
        return false;

    // ── ★ M 键：在"矩形选框 / 椭圆选框"之间轮换 ★ ──
    //
    // 为什么这条逻辑在 MainWindow 而不在 ToolManager：
    //   ToolManager::findByShortcut() 是"按 key 查工具"，命中第一个就返回，
    //   它表达不了"一个键对应两个工具"。而"按 M 就在两种选框之间来回切"
    //   描述的是**用户按键习惯**，属于界面策略，不是工具集合的职责。
    //   放在这里还能享受现有的事件派发顺序（工具快捷键优先于菜单）。
    //
    // 对齐 Photoshop：第一次按 M 进矩形选框，再按一次变椭圆选框，
    // 再按又变回来。用户不需要去看工具箱就知道两种形状怎么切。
    if (key == Qt::Key_M) {
        if (auto *selTool = qobject_cast<SelectionTool *>(m_toolManager->activeTool())) {
            const QString next =
                (selTool->kind() == SelectionTool::Kind::Rectangle)
                    ? QStringLiteral("ellipseselect")
                    : QStringLiteral("rectselect");
            return m_toolManager->setActiveTool(next);
        }
        // 当前不是选区工具 → 按常规路径切进矩形选框（它绑了 M）
        return m_toolManager->setActiveToolByShortcut(key);
    }

    // ── ★ U 键：在"矩形 / 椭圆"之间轮换 ★ ──
    //
    // 与上面的 M 键完全同构，理由也一样（"一个键对应两个工具"是界面策略，
    // 不是工具集合的职责）。这里只补一条与形状工具特有的约定：
    //
    //   ShapeTool 的三个 Kind 里，直线绑 L，而**矩形与椭圆故意都绑 U**。
    //   所以 findByShortcut(U) 一定命中矩形（它先注册），
    //   "再按一次换成椭圆"就必须在这里做 —— 见 ShapeTool::shortcutKey()。
    //
    // ★ 当前是直线工具时按 U 应该**进形状组**（矩形），而不是轮换 ★
    //   所以判断里显式排除 Line，落到最后的常规路径。
    if (key == Qt::Key_U) {
        if (auto *shapeTool = qobject_cast<ShapeTool *>(m_toolManager->activeTool())) {
            if (shapeTool->kind() != ShapeTool::Kind::Line) {
                const QString next =
                    (shapeTool->kind() == ShapeTool::Kind::Rectangle)
                        ? QStringLiteral("ellipse")
                        : QStringLiteral("rect");
                return m_toolManager->setActiveTool(next);
            }
        }
        return m_toolManager->setActiveToolByShortcut(key);
    }

    // 查一下有没有工具绑这个键。
    // ★ 不能直接调 setActiveToolByShortcut 然后看返回值 ★
    //   因为如果当前工具恰好就是目标工具，setActiveTool 会因为
    //   "已经相同"而提前返回，但 activateToolByShortcut 仍返回 true ——
    //   看起来没问题。真正的问题是：如果这个键**没有**绑工具，
    //   我们要返回 false 让事件继续传递下去。
    //   所以先 findByShortcut 查一下。
    if (!m_toolManager->findByShortcut(key))
        return false;

    return m_toolManager->setActiveToolByShortcut(key);
}

// ============================================================================
//  B. 编辑命令
// ============================================================================

void MainWindow::undo()
{
    if (m_commandStack)
        m_commandStack->undo();
}

void MainWindow::redo()
{
    if (m_commandStack)
        m_commandStack->redo();
}

// ---------------------------------------------------------------------------
//  撤销步数
// ---------------------------------------------------------------------------

void MainWindow::setUndoLimit(int limit)
{
    // CommandStack 内部也会夹一次。这里先夹，是为了让 m_undoLimit
    // 与真正生效的值永远相等 —— 否则存进设置的是越界值，下次启动还要再夹。
    m_undoLimit = qBound(1, limit, 200);

    if (!m_tabs)
        return;

    // 对**所有已打开的页**生效，不只是以后新开的页。
    for (int i = 0; i < m_tabs->count(); ++i) {
        DocumentTab *tab = m_tabs->tabAt(i);
        if (tab && tab->stack)
            tab->stack->setUndoLimit(m_undoLimit);
    }
}

void MainWindow::openUndoLimitDialog()
{
    bool ok = false;
    const int value = QInputDialog::getInt(
        this, tr("Undo Steps"), tr("How many undo steps to keep per document:"),
        m_undoLimit, 1, 200, 10, &ok);

    if (!ok || value == m_undoLimit)
        return;

    setUndoLimit(value);

    // 改完立刻落盘：用户可能直接关窗口，而那条路径上不保证再走一遍
    // saveSettings() 的全部分支。
    QSettings settings;
    settings.setValue(QStringLiteral("edit/undoLimit"), m_undoLimit);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Undo steps: %1").arg(m_undoLimit), 4000);
}

// ---------------------------------------------------------------------------
//  剪贴板：构造数据
// ---------------------------------------------------------------------------
//
//  ★★ 本项目文本里最重要的一处细节 ★★
//
//    复制时必须把**选区蒙版的值乘进 alpha 通道**。
//
//    为什么：用户框选一个椭圆后按 Ctrl+C，期望复制的是"椭圆内的内容"。
//    但图层位图里的像素是整张图的 —— 椭圆外的像素依然有颜色。
//    如果不做处理，粘贴出来的会是一个带周围背景的方块，
//    而不是那个椭圆。
//
//    做法：对每个像素，把它的 alpha 乘以选区蒙版值 / 255。
//    蒙版为 255（完全选中）时 alpha 不变；
//    蒙版为 128（半选中）时 alpha 减半；
//    蒙版为 0（未选中）时 alpha 归零 —— 即透明。
//    这样边界自然就是柔和的（羽化效果也一起带过去）。
//
//  ★ 另一个要点：裁剪到选区的包围盒 ★
//    复制整张 4000×3000 的图但只有中间 100×100 被选中时，
//    剪贴板里存一整张图是浪费（PNG 压缩后可能还有几 MB）。
//    裁剪到包围盒后，剪贴板数据通常只有几十 KB。
ClipboardData MainWindow::buildClipboardData() const
{
    ClipboardData data;

    if (!m_doc)
        return data;

    const Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isNull())
        return data;

    const Selection &selection = m_doc->selection();

    // ── 情况一：没有选区（全选状态）──
    //   直接复制整个图层，不需要碰 alpha。
    if (!selection.isActive()) {
        data.pixels = layer->image().copy();
        data.sourceBounds = layer->rect();
        data.sourceDocumentId = m_doc->id();
        return data;
    }

    // ── 情况二：有选区 ──
    //
    //  ① 取选区的包围盒（只在包围盒内工作，避免全图遍历）
    //  ② 把包围盒区域抠出来
    //  ③ 在原地把选区蒙版乘进 alpha
    //  ④ 再裁掉全透明的边缘（进一步减小数据量）
    const QRect bounds = selection.bounds().intersected(layer->rect());
    if (bounds.isEmpty()) {
        // 选区为空（用户可能框了一个 0 像素的区域）——
        // 返回无效数据，调用方会提示"没有可复制的内容"。
        return data;
    }

    // 抠出包围盒（用 copy 得到独立副本）
    data.pixels = layer->image().copy(bounds);
    data.sourceBounds = bounds;
    data.sourceDocumentId = m_doc->id();

    // ★ 转成非预乘格式再运算 ★
    //
    //   预乘格式下的 alpha 值本身就参与颜色了，直接乘 alpha 会导致
    //   "乘了两次" —— 结果是半透明区域的颜色明显偏暗。
    //   必须先转到非预乘空间。
    //
    //   这与"调整类运算必须做预乘往返"是同一条规律的另一个应用场景。
    QImage work = data.pixels.convertToFormat(QImage::Format_ARGB32);

    const QImage &mask = selection.mask();

    // 蒙版与画布同尺寸，需要按 bounds 偏移取样
    const int maskW = mask.width();
    const int maskH = mask.height();

    const int w = work.width();
    const int h = work.height();

    for (int y = 0; y < h; ++y) {
        // 画出像素行的起始指针 —— 比逐像素调用 setPixelColor 快一个数量级
        QRgb *line = reinterpret_cast<QRgb *>(work.scanLine(y));
        const int maskY = bounds.top() + y;
        if (maskY < 0 || maskY >= maskH)
            continue;

        const uchar *maskLine = mask.constScanLine(maskY);

        for (int x = 0; x < w; ++x) {
            const int maskX = bounds.left() + x;
            if (maskX < 0 || maskX >= maskW)
                continue;

            const int maskValue = int(maskLine[maskX]);
            if (maskValue == 255)
                continue;   // 完全选中，不做运算（这是最快的路径）

            const QRgb pixel = line[x];
            int a = qAlpha(pixel);

            // ★ 用整数乘法而不是浮点 ★
            //   (a * mask + 127) / 255 是"乘以 mask/255 并四舍五入"的
            //   整数等价形式。+127 是为了四舍五入（255/2 ≈ 127）。
            //   整数运算在这类逐像素循环里比浮点快 2~3 倍。
            a = (a * maskValue + 127) / 255;

            line[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), a);
        }
    }

    data.pixels = work;

    // ── 裁掉全透明的边缘 ──
    //
    //  例如用户框选一个圆形，包围盒是正方形，四个角都是透明的。
    //  裁掉它们能显著减小剪贴板数据（尤其是 PNG 压缩后）。
    //
    //  ★ 用 engine 层的 contentBounds ★
    //    它逐行扫描并跳过全透明行，比自己写一遍高效。
    const QRect tightBounds = transform::contentBounds(data.pixels);
    if (!tightBounds.isEmpty() && tightBounds != data.pixels.rect()) {
        data.pixels = data.pixels.copy(tightBounds);
        // ★ sourceBounds 也要跟着调整 ★
        //   否则"原位粘贴"会贴错位置 —— 少了左上角这个偏移。
        data.sourceBounds = QRect(bounds.topLeft() + tightBounds.topLeft(),
                                  tightBounds.size());
    }

    // ── 保留选区蒙版（可选）──
    //  用同样的包围盒抠出来。用途：粘贴后可以"载入选区"恢复原来的选区形状。
    if (!mask.isNull()) {
        data.mask = mask.copy(tightBounds.isEmpty() ? bounds : tightBounds)
                        .convertToFormat(QImage::Format_Grayscale8);
    }

    return data;
}

// ---------------------------------------------------------------------------
//  剪贴板：复制 / 剪切
// ---------------------------------------------------------------------------

void MainWindow::copySelection()
{
    if (!requireDocument())
        return;

    ClipboardData data = buildClipboardData();
    if (!data.isValid()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Nothing to copy"), 2500);
        return;
    }

    // ── 写入系统剪贴板 ──
    //
    //  ★ 两种格式一起写 ★
    //    自定义格式 —— 本程序自己粘贴时用，保留位置与蒙版信息
    //    image/png —— 其他程序（浏览器、聊天工具）能读
    //    只写自定义格式的话，用户没法把图片粘到微信里发出去。
    auto *mime = new QMimeData;

    // 1) 自定义格式（序列化后的完整数据）
    mime->setData(QLatin1String(kClipboardMimeLayer), data.serialize());

    // 2) 标准图像格式
    //    ★ 用 PNG 而不是 BMP ★
    //      BMP 是未压缩的，一张 4000×3000 进剪贴板有 48MB，
    //      会让系统剪贴板变慢、甚至拖累其他程序。
    //      PNG 压缩后通常几百 KB。
    {
        QByteArray pngBytes;
        QBuffer buffer(&pngBytes);
        buffer.open(QIODevice::WriteOnly);
        // 剪贴板用的 PNG 用较低质量（高压缩率）：剪贴板是无损中转，
        // 而且数据要驻留在系统里，体积越小越好。
        data.pixels.save(&buffer, "PNG", 50);
        buffer.close();

        if (!pngBytes.isEmpty())
            mime->setData(QStringLiteral("image/png"), pngBytes);
    }

    // 3) 也给一份 image/bmp —— 兼容只认 BMP 的老程序
    //    （某些老版本的 Office、画图工具）
    {
        QImage bmpImage = data.pixels.convertToFormat(QImage::Format_ARGB32);
        QByteArray bmpBytes;
        QBuffer buffer(&bmpBytes);
        buffer.open(QIODevice::WriteOnly);
        bmpImage.save(&buffer, "BMP");
        buffer.close();

        // ★ 只在数据不太大时才提供 ★
        //   48MB 的 BMP 会让剪贴板变得很慢。
        if (!bmpBytes.isEmpty() && bmpBytes.size() < 32 * 1024 * 1024)
            mime->setData(QStringLiteral("image/bmp"), bmpBytes);
    }

    QApplication::clipboard()->setMimeData(mime);

    if (m_statusBar) {
        m_statusBar->showMessage(
            tr("Copied %1 × %2 of content").arg(data.size().width()).arg(data.size().height()),
            2500);
    }
}

void MainWindow::cutSelection()
{
    if (!requireDocument())
        return;

    // ── 剪切 = 复制 + 删除 ──
    //
    //  ★ 顺序：先复制，再删除 ★
    //    反过来（先删再复制）会复制到空白内容 ——
    //    这种 bug 在第一次测试时就能发现，但在重构时很容易被引入。
    copySelection();

    if (!m_commandStack)
        return;

    // ── 删除选区内容（或整个图层内容）──
    Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isLocked())
        return;

    const QImage before = captureActiveLayerImage();

    // 用 QPainter 的 Clear 合成模式把选区内的像素清空。
    QImage after = before.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    {
        QPainter p(&after);
        p.setCompositionMode(QPainter::CompositionMode_Clear);

        const Selection &selection = m_doc->selection();
        if (selection.isActive()) {
            // 有选区：只清掉选区内的部分。
            //
            //  ★ 怎么用灰度蒙版做"按比例清除" ★
            //    Clear 模式会把 alpha 直接设为 0，无法表达"半透明白"。
            //    所以做法是：把蒙版当作 alpha 通道，构造一张
            //    "蒙版作为不透明度的黑色图"，然后 SourceOver 叠上去 ——
            //    不对，那是加黑色而不是清空。
            //
            //    正确做法：分离通道逐个处理。这里用最简单可靠的方案 ——
            //    在 CPU 上逐像素处理（避开 QPainter 的合成语义限制）。
            p.end();

            const QImage &mask = selection.mask();
            const QRect bounds = selection.bounds();
            const int w = after.width();
            const int h = after.height();

            for (int y = bounds.top(); y <= bounds.bottom() && y < h; ++y) {
                if (y < 0)
                    continue;
                QRgb *line = reinterpret_cast<QRgb *>(after.scanLine(y));
                const uchar *maskLine = mask.constScanLine(y);

                for (int x = bounds.left(); x <= bounds.right() && x < w; ++x) {
                    if (x < 0)
                        continue;

                    const int m = int(maskLine[x]);
                    if (m == 0)
                        continue;   // 选区外，不动

                    // 按蒙版值比例减小 alpha
                    const QRgb pixel = line[x];
                    const int a = qAlpha(pixel);
                    const int newA = (a * (255 - m) + 127) / 255;

                    // ★ 预乘格式下的改 alpha 必须同时缩 RGB ★
                    //   预乘语义要求 RGB ≤ alpha。如果只改 alpha 不缩 RGB，
                    //   会出现"RGB > alpha"的非法预乘值 ——
                    //   后续合成时颜色会溢出（表现为奇怪的亮斑）。
                    if (a > 0) {
                        const int newR = qRed(pixel)   * newA / a;
                        const int newG = qGreen(pixel) * newA / a;
                        const int newB = qBlue(pixel)  * newA / a;
                        line[x] = qRgba(newR, newG, newB, newA);
                    } else {
                        line[x] = qRgba(0, 0, 0, 0);
                    }
                }
            }
        } else {
            // 无选区：清空整个图层
            p.fillRect(after.rect(), Qt::transparent);
            p.end();
        }
    }

    pushActiveLayerImageCommand(before, after, tr("Cut"));

    if (m_statusBar)
        m_statusBar->showMessage(tr("Cut to the clipboard"), 2500);
}

// ---------------------------------------------------------------------------
//  剪贴板：粘贴
// ---------------------------------------------------------------------------

bool MainWindow::pasteFromClipboard(bool asNewLayer)
{
    if (!requireDocument())
        return false;

    if (!m_commandStack)
        return false;

    // ── 读剪贴板 ──
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The clipboard is empty"), 2500);
        return false;
    }

    ClipboardData data;

    // ★ 优先读自定义格式 ★
    //   它包含完整的像素 + 蒙版 + 原始位置信息。
    //   读 image/png 也能工作，但会丢失"原位粘贴"所需的位置信息，
    //   而且 PNG 往返会损失一点精度（虽然是无损的，但会多一次编解码开销）。
    if (mime->hasFormat(QLatin1String(kClipboardMimeLayer))) {
        data = ClipboardData::deserialize(
            mime->data(QLatin1String(kClipboardMimeLayer)));
    }

    // 降级：读标准图像格式
    if (!data.isValid() && mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            data.pixels = img.convertToFormat(QImage::Format_ARGB32);
            data.sourceBounds = QRect();   // 来源不明
            data.sourceDocumentId = 0;
        }
    }

    if (!data.isValid()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The clipboard has no usable image data"), 2500);
        return false;
    }

    // ── 计算粘贴位置 ──
    //
    //  ★ 三种情况 ★
    //    ① 来自本文档：原位粘贴（贴回原处）
    //    ② 来自其他程序但尺寸比画布小：居中
    //    ③ 比画布大：贴到左上角（用户能看到大部分内容，
    //       需要的话可以自己移动）
    const QSize canvasSize = m_doc->canvasSize();
    QPoint pastePos(0, 0);

    if (data.isFromDocument(m_doc->id())) {
        // ① 原位粘贴。
        //   这是"复制 → 撤销 → 粘贴"这个常用工作流的预期行为：
        //   内容应该回到它原来的位置，而不是跳到画面中央。
        pastePos = data.sourceBounds.topLeft();
    } else {
        // ②③ 居中或左上
        if (data.size().width() <= canvasSize.width()
            && data.size().height() <= canvasSize.height()) {
            pastePos = QPoint((canvasSize.width() - data.size().width()) / 2,
                              (canvasSize.height() - data.size().height()) / 2);
        } else {
            pastePos = QPoint(0, 0);
        }
    }

    const QRect targetRect(pastePos, data.size());

    // ── 粘贴为新图层 ──
    if (asNewLayer) {
        // ★ 宏在这里的作用是"统一撤销文本"，不是合并多条命令 ★
        //
        //   建图层本身只有一条 LayerCommand（它的构造函数直接吃
        //   before/after 两份图层列表），所以宏里只有一个孩子。
        //   保留宏是为了让历史面板显示"粘贴为新图层"这个**用户语义**，
        //   而不是内部的"新建图层"。
        //
        //   （原先的注释说"用宏把新建图层+写入像素合并成一次撤销"，
        //    那是早期两条命令时的说法，现在这条理由已经不成立了 ——
        //    但行为保持不变，只有注释被纠正。）
        m_commandStack->beginMacro(tr("Paste as New Layer"));

        // ★ 建图层的活全部交给唯一实现 ★
        //   与「打开为图层」共用同一份代码，见 addImageAsNewLayer()。
        addImageAsNewLayer(data.pixels, pastePos,
                           tr("Pasted Layer"), tr("New Layer"));

        m_commandStack->endMacro();

        if (m_statusBar) {
            m_statusBar->showMessage(
                tr("Pasted as a new layer (%1 × %2)")
                    .arg(data.size().width()).arg(data.size().height()), 2500);
        }

        return true;
    }

    // ── 粘贴到当前图层 ──
    Layer *layer = m_doc->activeLayer();
    if (!layer) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No active layer available"), 2500);
        return false;
    }

    if (layer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is locked - cannot paste"), 2500);
        return false;
    }

    const QImage before = captureActiveLayerImage();

    QImage after = before.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter p(&after);

        // ★ 有选区时限制粘贴范围 ★
        //   用选区蒙版作为裁剪区域。
        //
        //   ★ 怎么把灰度蒙版变成裁剪区域 ★
        //     QPainter::setClipRegion 只接受 QRegion（硬边缘），
        //     表达不了羽化选区的灰阶过渡。
        //
        //     做法：分两步
        //       ① 先用软边缘的选区蒙版把"粘贴内容"的 alpha 乘一遍
        //          （与复制时的处理对称）
        //       ② 裁剪区域用选区包围盒（粗粒度，只用于性能优化）
        //
        //     这样羽化边缘依然正确，而绘制范围也受限。
        const Selection &selection = m_doc->selection();
        if (selection.isActive()) {
            // 把选区蒙版乘进待粘贴内容的 alpha。
            //
            // 注意：这里要从 data.pixels 拷一份来处理，
            // 因为 data 是局部变量但仍然要用于别的地方（目前没有，
            // 但保持"不就地修改输入"的习惯更安全）。
            QImage clipped = data.pixels.convertToFormat(QImage::Format_ARGB32);
            const QImage &mask = selection.mask();

            const int w = clipped.width();
            const int h = clipped.height();

            for (int y = 0; y < h; ++y) {
                const int maskY = pastePos.y() + y;
                if (maskY < 0 || maskY >= mask.height())
                    continue;

                QRgb *line = reinterpret_cast<QRgb *>(clipped.scanLine(y));
                const uchar *maskLine = mask.constScanLine(maskY);

                for (int x = 0; x < w; ++x) {
                    const int maskX = pastePos.x() + x;
                    if (maskX < 0 || maskX >= mask.width())
                        continue;

                    const int m = int(maskLine[maskX]);
                    if (m == 255)
                        continue;

                    const QRgb pixel = line[x];
                    const int a = qAlpha(pixel);
                    line[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel),
                                    (a * m + 127) / 255);
                }
            }

            p.setClipRect(selection.bounds());
            p.drawImage(pastePos, clipped);
        } else {
            p.drawImage(pastePos, data.pixels);
        }

        p.end();
    }

    pushActiveLayerImageCommand(before, after, tr("Paste"));

    if (m_statusBar)
        m_statusBar->showMessage(tr("Pasted into the current layer"), 2500);

    return true;
}

void MainWindow::pasteAsNewLayer()
{
    pasteFromClipboard(true);
}

void MainWindow::pasteIntoLayer()
{
    pasteFromClipboard(false);
}

// ============================================================================
//  addImageAsNewLayer —— "把一张图变成一个新图层"的唯一实现
// ============================================================================
//
//  两条调用路径：
//    · 「编辑 → 粘贴为新图层」(Ctrl+V)  —— 像素来自剪贴板
//    · 「文件 → 打开为图层…」           —— 像素来自文件
//
//  ★ 两者要做的是同一件事，所以只能有一份实现 ★
//    "新图层要填满画布""用 CompositionMode_Source 而不是 SourceOver"
//    这两条约束如果各写一遍，改一处忘一处，而且第二份没有自检守着
//    （与本工程"合成只有 composeStroke 一处"是同一条纪律）。
//
//  ★ 为什么必须自己造一个"填满画布"的图层，而不是把 image 直接交给 addLayer ★
//    Document::addLayer(const Layer &, int) 在图层尺寸**大于画布**时，
//    会静默调用 setCanvasSizeInternal() 去扩大画布 —— 那是一条
//    **不压任何命令**的路径，撤销时画布尺寸回不去。
//    本函数先把位图开成画布大小、再把图像画进去，"图层尺寸恒等于画布尺寸"
//    这条不变量就守住了，那条静默路径永远不会被踩到。
//    （自检里有一条断言专门盯它：新图层尺寸必须 == 画布尺寸。）
bool MainWindow::addImageAsNewLayer(const QImage &image, const QPoint &pos,
                                    const QString &layerName,
                                    const QString &historyText)
{
    if (!m_doc || !m_commandStack || image.isNull())
        return false;

    const QSize canvasSize = m_doc->canvasSize();

    // 建层前的快照 —— LayerCommand 吃的是"整份图层列表"的前后对照
    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    Layer newLayer(canvasSize, layerName);

    {
        QImage canvasImage(canvasSize, QImage::Format_ARGB32_Premultiplied);
        canvasImage.fill(Qt::transparent);

        QPainter p(&canvasImage);
        // ★ 用 Source 而不是 SourceOver ★
        //   SourceOver 会把图像与"透明底色"混合 —— 全透明底色下结果
        //   虽然一样，但 Source 的语义更直接：就是"覆盖上去"。
        //   而且将来若底色改成不透明（例如"导入时铺白底"），
        //   SourceOver 会让整张图被底色稀释，Source 不会。
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(pos, image);
        p.end();

        newLayer.setImage(canvasImage);
    }

    const int newIndex = m_doc->addLayer(newLayer, -1);   // -1 = 加到最上面

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    m_commandStack->push(new LayerCommand(m_doc, LayerCommand::Op::Add,
                                          beforeLayers, afterLayers,
                                          activeBefore, newIndex,
                                          historyText));

    // 让新图层成为活动图层。
    // ★ 不依赖 addLayer 内部的"顺便设活动层" ★
    //   那是实现细节；这里显式写出来，语义才不依赖别处的行为
    //   （后续操作——框选、移动——必须作用在这个新图层上）。
    m_doc->setActiveLayerIndex(newIndex);

    return true;
}

// ---------------------------------------------------------------------------
//  删除选区内容
// ---------------------------------------------------------------------------

void MainWindow::onDeleteSelection()
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is not editable"), 2500);
        return;
    }

    const QImage before = captureActiveLayerImage();
    QImage after = before.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const Selection &selection = m_doc->selection();

    if (selection.isActive()) {
        // ★ 与剪切相同的逐像素路径 ★
        //   抽成公共函数会更整洁，但这里保留两份的原因：
        //   剪切是"复制 + 删除"的一个复合操作，它用的 before 与
        //   删除用的 before 必须是**同一个**（否则撤销时会回到不同的状态）。
        //   强行抽取会引入一个"传入 before"的参数，反而更绕。
        const QImage &mask = selection.mask();
        const QRect bounds = selection.bounds().intersected(after.rect());
        const int w = after.width();
        const int h = after.height();

        for (int y = bounds.top(); y <= bounds.bottom() && y < h; ++y) {
            if (y < 0)
                continue;
            QRgb *line = reinterpret_cast<QRgb *>(after.scanLine(y));
            const uchar *maskLine = mask.constScanLine(y);

            for (int x = bounds.left(); x <= bounds.right() && x < w; ++x) {
                if (x < 0)
                    continue;

                const int m = int(maskLine[x]);
                if (m == 0)
                    continue;

                const QRgb pixel = line[x];
                const int a = qAlpha(pixel);
                const int newA = (a * (255 - m) + 127) / 255;

                if (a > 0) {
                    line[x] = qRgba(qRed(pixel)   * newA / a,
                                    qGreen(pixel) * newA / a,
                                    qBlue(pixel)  * newA / a,
                                    newA);
                } else {
                    line[x] = qRgba(0, 0, 0, 0);
                }
            }
        }

        pushActiveLayerImageCommand(before, after, tr("Delete Selection Content"));
    } else {
        // 无选区：清空整个图层的内容（保留图层本身）
        QImage cleared(after.size(), QImage::Format_ARGB32_Premultiplied);
        cleared.fill(Qt::transparent);

        pushActiveLayerImageCommand(before, cleared, tr("Clear Layer"));
    }
}

// ---------------------------------------------------------------------------
//  选区命令
// ---------------------------------------------------------------------------

void MainWindow::selectAll()
{
    if (!requireDocument() || !m_commandStack)
        return;

    // ★ "全选"在实现上就是"清空选区" ★
    //   Selection 的内部表示中，"没有选区"等价于"全选" ——
    //   因为所有操作都可以作用于整张图，蒙版运算可以完全跳过。
    //   这个设计让"最常见的场景（无选区）"变成零成本。
    if (!m_doc->selection().isActive()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Already in the select-all state"), 2000);
        return;
    }

    const Selection before = m_doc->selection().clone();

    m_doc->selection().selectAll();
    m_doc->markDirty();

    const Selection after = m_doc->selection().clone();

    auto *cmd = new SelectionCommand(m_doc, before, after, tr("Select All"));
    m_commandStack->push(cmd);

    if (m_canvas)
        m_canvas->update();
}

void MainWindow::deselect()
{
    if (!requireDocument() || !m_commandStack)
        return;

    if (!m_doc->selection().isActive()) {
        // 已经是"无选区"状态，不需要记录一条撤销
        // （记了的话用户按 Ctrl+D 会往撤销栈里塞无意义的条目）
        return;
    }

    const Selection before = m_doc->selection().clone();

    m_doc->selection().clear();
    m_doc->markDirty();

    const Selection after = m_doc->selection().clone();

    auto *cmd = new SelectionCommand(m_doc, before, after, tr("Deselect"));
    m_commandStack->push(cmd);

    if (m_canvas)
        m_canvas->update();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Selection cancelled"), 2000);
}

void MainWindow::invertSelection()
{
    if (!requireDocument() || !m_commandStack)
        return;

    // ★ 没有选区时"反选"是个死胡同，必须提前拦下 ★
    //
    //   本工程的语义是"没有选区 == 全选"。若允许在全选状态下反选，
    //   结果应该是"什么都没选中"—— 而"什么都没选中"在数据结构里
    //   只能表达成一份全 0 的蒙版。那会让每个工具都走一遍
    //   "逐像素乘蒙版"的慢路径，最终却什么也写不进去（性能坑）。
    //   Selection::invert() 自己也做了提前返回，但在这里拦一道额外
    //   避免往撤销栈里塞一条 before == after 的空命令 ——
    //   用户按了 Ctrl+Shift+I 却什么都没发生，Ctrl+Z 还得多按一次，
    //   这种"无效果却占一条历史"的记录很招人烦。
    if (!m_doc->selection().isActive()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No selection to invert"), 2000);
        return;
    }

    const Selection before = m_doc->selection().clone();

    m_doc->selection().invert();
    m_doc->markDirty();

    const Selection after = m_doc->selection().clone();

    auto *cmd = new SelectionCommand(m_doc, before, after, tr("Invert Selection"));
    m_commandStack->push(cmd);

    if (m_canvas)
        m_canvas->update();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Selection inverted"), 2000);
}

// ============================================================================
//  C. 调整与几何
// ============================================================================

void MainWindow::applyGrayscale()
{
    // ── 检查 ──
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is not editable"), 2500);
        return;
    }

    // ── 记录 before ──
    const QImage before = captureActiveLayerImage();

    // ── 执行 ──
    //
    //  ★ 有选区时只处理选区内的像素 ★
    //
    //    怎么实现"只处理选区"？有两种方案：
    //      ① 在 engine 的每个函数里都传蒙版参数（侵入性大，
    //         而且要为每个滤镜都实现一遍蒙版混合逻辑）
    //      ② 先对全图运算，再把结果按蒙版混合回原图
    //         （一次额外遍历，但完全复用现有的纯函数）
    //
    //    这里选 ②。理由：engine 层的函数保持"纯函数、无蒙版概念"的
    //    简洁设计，而"选区的语义"统一由上层处理 —— 这与
    //    "所有写入像素的工具必须先与选区蒙版求交"（约定 2）一致，
    //    只是这里的"求交"发生在 apply 之后。
    //
    //    ★ 性能代价 ★
    //      多一次全图遍历（1200 万像素约 15ms）。
    //      对滤镜这类本来就要几百毫秒的操作可以忽略。
    //      真正高频的画笔操作不会走这条路径（它直接用脏矩形 + 蒙版求交）。
    QImage result = filters::grayscale(before);

    // ── 按选区混合 ──
    const Selection &selection = m_doc->selection();
    if (selection.isActive()) {
        result = blendWithSelection(before, result, selection);
    }

    // ── 提交 ──
    pushActiveLayerImageCommand(before, result, tr("Desaturate"));

    if (m_statusBar)
        m_statusBar->showMessage(tr("Desaturate applied"), 2000);
}

void MainWindow::applyInvert()
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is not editable"), 2500);
        return;
    }

    const QImage before = captureActiveLayerImage();

    QImage result = filters::invert(before);

    const Selection &selection = m_doc->selection();
    if (selection.isActive())
        result = blendWithSelection(before, result, selection);

    pushActiveLayerImageCommand(before, result, tr("Invert"));

    if (m_statusBar)
        m_statusBar->showMessage(tr("Invert applied"), 2000);
}

// ============================================================================
//  blendWithSelection —— 按选区蒙版混合"处理前"与"处理后"的位图
// ============================================================================
//
//  这是"只处理选区内像素"的通用实现。所有整层滤镜/调整都复用它。
//
//  ★ 为什么不直接改 engine 的函数让它们接收蒙版 ★
//
//    engine 层的设计铁律是"纯函数、无副作用、只依赖 QtGui"。
//    "选区"是 core 层的概念（它有自己的类与生命周期管理）。
//    如果让 engine 认识选区，就会出现"engine 依赖 core"的反向依赖，
//    而且每个滤镜函数都要复习一遍"蒙版怎么用"。
//
//    在上层做混合则让 engine 保持干净，而且这套混合逻辑只需要写一次。
//
//  ★ 混合公式 ★
//
//    结果 = 原图 · (1 − m) + 处理后 · m      （m 为归一化的蒙版值）
//
//    这等价于"用蒙版作为 alpha 在两张图之间做 lerp"。
//    蒙版为 255 时完全取处理后的；为 0 时完全取原图；
//    中间值则按比例混合 —— 这正是羽化选区应有的行为。
QImage MainWindow::blendWithSelection(const QImage &before,
                                      const QImage &after,
                                      const Selection &selection)
{
    return blendByMask(before, after, selection.mask(), selection.bounds());
}

// ---------------------------------------------------------------------------
//  blendByMask —— 按蒙版混合的底层实现
// ---------------------------------------------------------------------------
//  与 blendWithSelection 的关系：后者只是把 Selection 拆成"蒙版 + 范围"
//  再调用本函数。拆开的目的见头文件：调整预览用的是**降采样**蒙版，
//  那时没有 Selection 对象可用，但必须复用同一段混合代码。
// ---------------------------------------------------------------------------
QImage MainWindow::blendByMask(const QImage &before,
                               const QImage &after,
                               const QImage &mask,
                               const QRect &maskBounds)
{
    if (before.size() != after.size())
        return after;

    // 没有蒙版（或蒙版为空）等价于"全选" —— 直接返回处理结果。
    // ★ 这个兜底必须写 ★
    //   否则下面会访问 mask.width()，一张空图的尺寸是 0×0，
    //   循环条件虽然不会成立，但"空蒙版"与"全选"的语义差别会
    //   让调用方难以判断自己拿到的到底是什么。
    //   注意 QImage 没有 isEmpty()（那是 QSize/QString 的接口），
    //   判空要用 isNull()，判"零尺寸"要自己看宽高。
    if (mask.isNull() || mask.width() == 0 || mask.height() == 0)
        return after;

    // ★ 用非预乘空间做混合 ★
    //   预乘空间下的线性插值是"对乘过 alpha 的值插值"，
    //   对全不透明的像素与半透明像素会得到不一致的结果
    //   （半透明区域会出现色偏）。
    QImage result = before.convertToFormat(QImage::Format_ARGB32);
    const QImage processed = after.convertToFormat(QImage::Format_ARGB32);

    const QRect bounds = maskBounds.intersected(result.rect());

    const int w = result.width();
    const int h = result.height();
    const int maskW = mask.width();
    const int maskH = mask.height();

    for (int y = bounds.top(); y <= bounds.bottom() && y < h; ++y) {
        if (y < 0 || y >= maskH)
            continue;

        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(processed.constScanLine(y));
        const uchar *maskLine = mask.constScanLine(y);

        for (int x = bounds.left(); x <= bounds.right() && x < w; ++x) {
            if (x < 0 || x >= maskW)
                continue;

            const int m = int(maskLine[x]);
            if (m == 0)
                continue;   // 选区外：保持原样
            if (m == 255) {
                dstLine[x] = srcLine[x];   // 选区内：直接取处理结果
                continue;
            }

            // 中间值：按比例插值
            const QRgb a = dstLine[x];
            const QRgb b = srcLine[x];
            const int inv = 255 - m;

            // (a*inv + b*m + 127) / 255 —— 四舍五入的整数插值
            dstLine[x] = qRgba((qRed(a)   * inv + qRed(b)   * m + 127) / 255,
                               (qGreen(a) * inv + qGreen(b) * m + 127) / 255,
                               (qBlue(a)  * inv + qBlue(b)  * m + 127) / 255,
                               (qAlpha(a) * inv + qAlpha(b) * m + 127) / 255);
        }
    }

    return result;
}

// ============================================================================
//  几何操作（旋转 / 翻转）
// ============================================================================

bool MainWindow::performGeometryOp(GeoOp op)
{
    if (!m_doc || !m_commandStack)
        return false;

    // ── 前置检查 ──
    if (m_doc->layerCount() == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No layer to operate on"), 2500);
        return false;
    }

    // ── 锁定检查 ──
    //
    // ★ 几何操作的锁定语义 ★
    //   几何操作影响**所有**图层。如果其中任何一个是锁定的，
    //   严格来说应该拒绝执行 —— 否则用户的意图是"保护这一层不动"，
    //   而几何操作把它一起转了，破坏了保护。
    //
    //   但直接拒绝也会让"只有一个锁定图层"的情况完全无法旋转整图。
    //   折中：只要**活动图层**未锁定就允许（用户显式操作的那一层），
    //   并在状态栏提示"其他锁定的图层也会被一起变换"。
    const Layer *activeLayer = m_doc->activeLayer();
    if (activeLayer && activeLayer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is locked - geometry operations are not allowed"), 3000);
        return false;
    }

    // ── 记录 before 快照 ──
    //
    // ★ 用 GeometryChangeCommand::captureState ★
    //   它内部用 cloneAll() 做浅拷贝（QImage 共享内存），所以快照本身很轻；
    //   真正的内存开销在命令构造时的 qCompress。
    const GeometryChangeCommand::Snapshot before =
        GeometryChangeCommand::captureState(m_doc);

    // ── 执行操作 ──
    bool ok = false;
    GeometryChangeCommand::Kind kind = GeometryChangeCommand::Kind::Rotate90;
    QString text;

    switch (op) {
    case GeoOp::Rotate90CW:
        ok = m_doc->rotateImage90(true);
        kind = GeometryChangeCommand::Kind::Rotate90;
        text = tr("Rotate 90° Clockwise");
        break;

    case GeoOp::Rotate90CCW:
        ok = m_doc->rotateImage90(false);
        kind = GeometryChangeCommand::Kind::Rotate90;
        text = tr("Rotate 90° Counterclockwise");
        break;

    case GeoOp::Rotate180:
        // ★ 180° 用两次 90° 实现 ★
        //   为什么不直接写一个 rotateImage180？
        //   因为两次 90° 是**无损**的（整数坐标重排，无插值），
        //   而单独实现的 180° 如果走通用的任意角度路径就会有插值损失。
        //   复用 90° 的实现可以保证"转四次回到原点、逐像素一致"，
        //   这是阶段四的验收标准之一。
        ok = m_doc->rotateImage90(true);
        if (ok)
            ok = m_doc->rotateImage90(true);
        kind = GeometryChangeCommand::Kind::Rotate90;
        text = tr("Rotate 180°");
        break;

    case GeoOp::FlipH:
        ok = m_doc->flipImage(FlipDirection::Horizontal);
        kind = GeometryChangeCommand::Kind::Flip;
        text = tr("Flip Horizontal");
        break;

    case GeoOp::FlipV:
        ok = m_doc->flipImage(FlipDirection::Vertical);
        kind = GeometryChangeCommand::Kind::Flip;
        text = tr("Flip Vertical");
        break;
    }

    if (!ok) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Geometry operation failed"), 3000);
        return false;
    }

    return commitGeometryChange(before, kind, text);
}

bool MainWindow::commitGeometryChange(const GeometryChangeCommand::Snapshot &before,
                                      GeometryChangeCommand::Kind kind,
                                      const QString &text)
{
    if (!m_doc || !m_commandStack)
        return false;

    // ── 记录 after 并提交 ──
    const GeometryChangeCommand::Snapshot after =
        GeometryChangeCommand::captureState(m_doc);

    auto *cmd = new GeometryChangeCommand(
        m_doc,
        before.layers, after.layers,
        before.canvasSize, after.canvasSize,
        before.selection, after.selection,
        before.activeIndex, after.activeIndex,
        kind, text);

    // ── 内存预检 ──
    //
    // ★ 这是内存控制的关键一环 ★
    //   GeometryChangeCommand 要存**所有图层的**前后两份位图。
    //   4 图层 × 4000×3000 = 384MB 一步 —— 几十步就能把内存吃光。
    //
    //   如果压缩后仍然超过阈值（默认 256MB），就"执行操作但不记录撤销"，
    //   并明确告知用户。这比让程序因为内存耗尽而崩溃好得多。
    if (!cmd->isWorthRecording()) {
        delete cmd;   // 不压栈，自己删除

        m_doc->markDirty();
        if (m_canvas)
            m_canvas->invalidateComposite();

        QMessageBox::information(this, tr("Undo Not Recorded"),
            tr("The \"%1\" operation moved too much data, above the "
               "undo memory limit,\n"
               "so it <b>cannot be undone</b>.\n\n"
               "The image has been modified. To keep the original, do "
               "not save - reopen the file instead.").arg(text));

        return true;
    }

    m_commandStack->push(cmd);

    // ★ 让画布适合窗口 ★
    //
    //  几何操作会改变画布尺寸，原来的视图缩放可能变得不合适
    //  （例如竖图旋转后变成横图，两边会超出窗口；图像放大后更是如此）。
    //
    //  ★ 但不应该每次都 zoomToFit ★
    //    用户在 400% 下仔细看一个角落，操作后希望还在那个角落附近。
    //    折中：只在"画布不再完全可见"时才适合窗口。
    if (m_canvas) {
        const QRectF visible = m_canvas->visibleImageRect();
        const QRectF canvasRect(QPointF(0, 0), QSizeF(m_doc->canvasSize()));

        if (!visible.contains(canvasRect))
            m_canvas->zoomToFit();
        else
            m_canvas->invalidateComposite();
    }

    if (m_statusBar) {
        m_statusBar->showMessage(
            tr("%1 applied (canvas %2 × %3)")
                .arg(text).arg(m_doc->canvasSize().width()).arg(m_doc->canvasSize().height()),
            3000);
    }

    return true;
}

// ============================================================================
//  尺寸类操作：图像大小 / 画布大小
// ============================================================================
//
//  ★ 这两个入口与旋转/翻转共用 commitGeometryChange() ★
//    它们的差别只在"执行那一步调用 Document 的哪个方法"。
//
//  ★ 都不预览，确定时才一次性运算 ★
//    理由见 ImageSizeDialog.h 开头：尺寸类操作不是"参数 → 像素映射"，
//    没法用调整类对话框那套降采样预览机制；在副本上做又会因为副本本身
//    更小、放回屏幕时再插值一次，让用户看到与真实结果不符的画面。
//
//  ★ 前置检查比旋转更严 ★
//    旋转是纯坐标重排，失败最多是画布没变；尺寸类操作要**重新分配**
//    整幅位图，4000×3000 双三次约 180ms 且要额外一份同尺寸缓冲。
//    所以这里额外拒绝"任何图层被锁定"——几何操作会改写所有图层，
//    锁定了一层的用户期望那一层被保护，而不是"内容一起被缩放"。
// ============================================================================

void MainWindow::openImageSize()
{
    if (!m_doc || !m_commandStack)
        return;

    if (m_doc->layerCount() == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No layer to operate on"), 2500);
        return;
    }

    // ★ 锁定检查：这里用"任意图层锁定即拒绝" ★
    //   与旋转的"只看活动图层"不同。理由是代价不对称：
    //   缩放会让**所有**图层同时被重采样一次（内容会变糊），
    //   而"锁定"这一层的语义是"别碰这一层的内容"。宁可让用户先解锁，
    //   也不要静默地把他保护起来的那一层也重采样掉。
    const LayerList &layers = m_doc->layers();
    for (int i = 0; i < layers.count(); ++i) {
        const Layer *l = layers.layer(i);
        if (l && l->isLocked()) {
            if (m_statusBar)
                m_statusBar->showMessage(
                    tr("Layer \"%1\" is locked - unlock it before resizing the image").arg(l->name()), 3500);
            return;
        }
    }

    ImageSizeDialog dlg(m_doc->canvasSize(), ResampleMethod::Bicubic,
                        m_doc->layerCount(), this);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // ★ 用户看了一眼就点确定的情形已经被对话框挡掉了（确定按钮置灰）★
    //   这里再挡一道：宁可多一个判断，也不要在历史面板里留下一条
    //   什么都没改、却占着几百 MB 快照的记录。
    if (!dlg.isChanged())
        return;

    // ★ 快照必须在**修改之前**取 ★
    //   快照本身是浅拷贝（QImage 共享），取一次很便宜；但一旦在修改
    //   之后取，"before" 就变成了修改后的状态，撤销将完全失效 ——
    //   而且是那种"撤销了但看起来没变"的隐性失效。
    const GeometryChangeCommand::Snapshot before =
        GeometryChangeCommand::captureState(m_doc);

    if (!m_doc->scaleImage(dlg.newSize(), dlg.method())) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to resize the image (the new size may be too large)"), 3000);
        return;
    }

    commitGeometryChange(before, GeometryChangeCommand::Kind::ScaleImage,
                         tr("Image Size"));
}

void MainWindow::openCanvasSize()
{
    if (!m_doc || !m_commandStack)
        return;

    if (m_doc->layerCount() == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No layer to operate on"), 2500);
        return;
    }

    for (int i = 0; i < m_doc->layers().count(); ++i) {
        const Layer *l = m_doc->layers().layer(i);
        if (l && l->isLocked()) {
            if (m_statusBar)
                m_statusBar->showMessage(
                    tr("Layer \"%1\" is locked - unlock it before resizing the canvas").arg(l->name()), 3500);
            return;
        }
    }

    // ★ 内容包围盒要在这里取，传进对话框 ★
    //   对话框要用它算智能适配偏移、裁切量与示意图。取一次就够 ——
    //   对话框是模态的，期间文档不可能变化。
    const QRect content = m_doc->contentBounds();

    CanvasSizeDialog dlg(m_doc->canvasSize(), content, Anchor::Center, this);

    if (dlg.exec() != QDialog::Accepted)
        return;

    if (!dlg.isChanged())
        return;

    const GeometryChangeCommand::Snapshot before =
        GeometryChangeCommand::captureState(m_doc);

    // ★ smart 传 true：智能适配是产品的既定行为 ★
    //   （docs 里已定案）缩小时自动平移内容以免裁切，对话框会把
    //   这次平移明确告知用户。
    if (!m_doc->resizeCanvas(dlg.newSize(), dlg.anchor(),
                             dlg.fillColor(), /*smart=*/true)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to resize the canvas"), 3000);
        return;
    }

    commitGeometryChange(before, GeometryChangeCommand::Kind::ResizeCanvas,
                         tr("Canvas Size"));
}

// ============================================================================
//  任意角度旋转
// ============================================================================
//
//  ★ 唯一一个有对话框与实时预览的几何操作 ★
//    90° 倍数是整数坐标重排，无损、也不需要用户做选择；任意角度必然
//    重采样、且**不可逆**（转 15° 再转 -15° 只会多糊一次），所以必须先给预览。
//
//  ★ 预览复用调整类那套"降采样 + apply()"的通道 ★
//    差别只有两点：
//      ① 预览源是**合成图**而不是活动图层 —— 旋转是整幅图的操作，
//         用户看的是所有图层叠在一起的效果；
//      ② 预览要连**画布尺寸**一起交给画布 —— 旋转会把画布撑大。
//
//  ★ 提交走 Document::rotateImageArbitrary()，而不是 apply() ★
//    apply() 的契约是"单张图 → 单张图"，而旋转要改所有图层 + 画布尺寸，
//    还要能被一条 GeometryChangeCommand 撤销。好处是 Document 内部对
//    90° 倍数会走无损路径 —— 用户在对话框里输入 90 也能得到逐像素
//    相同的无损结果，不会被白糊一次。
//    两边最终都调 geometry::rotateImage()，所以"预览与结果一致"没有被破坏。
//
//  ★ 取消时必须连视图一起还原 ★
//    预览期间画布被撑大、视图被 zoomToFit 过。只调 clearPreviewImage()
//    的话，画布尺寸回到文档的、缩放却停在"适配大画布"的那一档 ——
//    用户一取消就发现图突然变小了。
// ============================================================================

void MainWindow::openRotateArbitrary()
{
    if (!m_doc || !m_commandStack || !m_canvas)
        return;

    if (m_doc->layerCount() == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No layer to operate on"), 2500);
        return;
    }

    // ★ 锁定检查与尺寸类操作同一条规矩：任意图层锁定即拒绝 ★
    //   旋转会重采样**所有**图层。"锁定"这一层的语义是"别碰它的内容"，
    //   而重采样是不可逆的 —— 宁可让用户先解锁，也不要静默地动它。
    const LayerList &layers = m_doc->layers();
    for (int i = 0; i < layers.count(); ++i) {
        const Layer *l = layers.layer(i);
        if (l && l->isLocked()) {
            if (m_statusBar)
                m_statusBar->showMessage(
                    tr("Layer \"%1\" is locked - unlock it before rotating the image").arg(l->name()), 3500);
            return;
        }
    }

    // ── 预览基准：降采样后的合成图 ──
    const QImage composite = m_doc->composite();
    if (composite.isNull()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Cannot generate a preview: the image content is empty"), 3000);
        return;
    }

    const QSize sz = composite.size();
    const int longest = qMax(sz.width(), sz.height());
    if (longest > kAdjustPreviewMaxSide) {
        const QSize previewSize(qRound(sz.width()  * qreal(kAdjustPreviewMaxSide) / longest),
                                qRound(sz.height() * qreal(kAdjustPreviewMaxSide) / longest));
        m_rotatePreviewBase = composite.scaled(previewSize, Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
    } else {
        // 小图不降采样：预览就是最终质量，也不必付缩放的代价
        m_rotatePreviewBase = composite;
    }

    // ── 记住打开前的视图，供取消时还原 ──
    const qreal   savedZoom   = m_canvas->zoom();
    const QPointF savedOffset = m_canvas->offset();

    QScopedPointer<RotateImageDialog> guard(
        new RotateImageDialog(m_doc->canvasSize(), m_rotatePreviewBase,
                              ResampleMethod::Bicubic, m_backgroundColor, this));
    RotateImageDialog *dialog = guard.data();
    m_rotateDialog = dialog;

    connect(dialog, &AdjustmentDialog::previewNeeded,
            this, &MainWindow::updateRotatePreview);

    // 建立初始预览：角度为 0，画面看起来和原来一样 ——
    // 但先走一遍能提前确认"预览通道是通的"，而不是等用户拖了滑块
    // 才发现毫无反应（那种无声的失效最容易让人以为功能没做）。
    updateRotatePreview();

    const int result = dialog->exec();

    // ★ 先断信号再清状态 ★
    //   对话框析构时其子控件仍可能发 valueChanged，届时 m_rotateDialog
    //   已被清空 —— 槽里的空判断虽能挡住，但从源头断开更稳妥。
    disconnect(dialog, nullptr, this, nullptr);
    m_rotateDialog = nullptr;

    if (result == QDialog::Accepted && dialog->isChanged()) {
        // ★ 顺序不能反：先清预览，再改文档 ★
        //   若先压命令，命令触发的重绘会被"预览图优先"的分支吃掉，
        //   用户看到的是"点确定之后图变糊了"（预览图是降采样的）。
        m_canvas->clearPreviewImage();

        // 快照必须在修改之前取：一旦在修改之后取，"before" 就成了修改后
        // 的状态，撤销会静默失效（表现为"撤销了但看起来没变"）。
        const GeometryChangeCommand::Snapshot before =
            GeometryChangeCommand::captureState(m_doc);

        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = m_doc->rotateImageArbitrary(dialog->angle(),
                                                   dialog->method(),
                                                   dialog->fillColor());
        QApplication::restoreOverrideCursor();

        if (!ok) {
            if (m_statusBar)
                m_statusBar->showMessage(tr("Rotation failed"), 3000);
            // 预览已清、文档没变，把视图拉回可用状态（预览期把它 fit 过了）
            m_canvas->setZoomLevel(savedZoom);
            m_canvas->setOffset(savedOffset);
        } else {
            commitGeometryChange(before, GeometryChangeCommand::Kind::RotateArbitrary,
                                 tr("Rotate Image"));
        }
    } else {
        // 取消 / 角度仍为 0：文档从头到尾没被碰过，不需要任何还原 ——
        // 这正是"预览不进 Document"的收益。只需把呈现层与视图复位。
        m_canvas->clearPreviewImage();
        m_canvas->setZoomLevel(savedZoom);
        m_canvas->setOffset(savedOffset);
    }

    m_rotatePreviewBase = QImage();
}

// ---------------------------------------------------------------------------
//  updateRotatePreview —— 重算降采样旋转结果并交给画布
// ---------------------------------------------------------------------------
void MainWindow::updateRotatePreview()
{
    if (!m_rotateDialog || !m_canvas)
        return;

    // 用户取消了"预览"勾选：把画布切回文档的真实内容。
    // 参数保持不动 —— 于是"关掉预览对比一下、再打开"是零成本的，
    // 这也是 Photoshop 里"预览"复选框的语义。
    if (!m_rotateDialog->isPreviewEnabled()) {
        m_canvas->clearPreviewImage();
        return;
    }

    // ★ 每一帧都从预览基准图重算 ★
    //   绝不在"上一次的预览结果"上继续旋转：每次旋转都伴随一次重采样，
    //   误差会随拖动次数累积，图会越来越糊 —— 而旋转比 LUT 变换严重得多
    //   （见 docs/画布与文件操作设计.md 6.5）。
    const QImage rendered = m_rotateDialog->apply(m_rotatePreviewBase);
    if (rendered.isNull()) {
        m_canvas->clearPreviewImage();
        return;
    }

    // ★ 连"旋转后的画布尺寸"一起交给画布 ★
    //   预览图是降采样的、与新画布不等大，画布按两者比例换算即可。
    //   不传这个尺寸，画布就会拿文档的旧尺寸去呈现 —— 旋转后撑到画布
    //   之外的那四个角会被旧取景框裁掉，而真实结果里那是填充区，
    //   用户会以为"旋转把内容裁掉了"。
    m_canvas->setPreviewImage(rendered, m_rotateDialog->newCanvasSize());
}

void MainWindow::rotate90CW()   { performGeometryOp(GeoOp::Rotate90CW);  }
void MainWindow::rotate90CCW()  { performGeometryOp(GeoOp::Rotate90CCW); }
void MainWindow::rotate180()    { performGeometryOp(GeoOp::Rotate180);   }
void MainWindow::flipHorizontal(){ performGeometryOp(GeoOp::FlipH);      }
void MainWindow::flipVertical() { performGeometryOp(GeoOp::FlipV);       }

// ============================================================================
//  裁剪
// ============================================================================
//
//  ★ 两个入口，一段收尾 ★
//
//    工具箱的裁剪工具在画布上拖出矩形后发 cropRequested()；
//    菜单「图像 → 裁剪」取当前选区的包围盒。两者最后都落到
//    applyCropFromTool()，再走 commitGeometryChange() 这条所有几何操作
//    共用的收尾（快照 → 内存预检 → 压栈 → 视图适配 → 状态栏）。
//
//  ★ 为什么工具不能自己压命令 ★
//    commitGeometryChange() 里那道"压缩后仍超 256MB 就不记录撤销"的预检
//    只能有一个入口（见 MainWindow.h 里它的注释）。工具自己 new 一个
//    GeometryChangeCommand 就等于把这道防线复制了一份，而漏掉的代价
//    要到内存耗尽时才暴露。工具与 ui 层之间只用信号通信，
//    这也是 tools → ui 唯一允许的方向。
//
//  ★ 裁剪是**破坏性**的（区域外像素丢弃，可撤销） ★
//    理由见 docs/画布与文件操作设计.md 6.7 末尾：本工程有一条隐含不变量
//    "图层位图尺寸恒等于画布尺寸"（画笔落点、滤镜遍历、选区蒙版全按画布
//    坐标索引图层像素）。"保留区域外内容"要求图层大于画布，会同时波及
//    这三处，不是裁剪能单独承担的改动。
// ============================================================================

void MainWindow::applyCropFromTool(const QRect &rect)
{
    if (!m_doc || !m_commandStack)
        return;

    if (m_doc->layerCount() == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("No layer to operate on"), 2500);
        return;
    }

    // ★ 锁定检查用"任意图层锁定即拒绝" ★
    //   与图像大小 / 画布大小同一档：裁剪会改写**所有**图层的位图尺寸，
    //   而"锁定"这一层的语义正是"别碰这一层"。宁可让用户先解锁。
    const LayerList &layers = m_doc->layers();
    for (int i = 0; i < layers.count(); ++i) {
        const Layer *l = layers.layer(i);
        if (l && l->isLocked()) {
            if (m_statusBar)
                m_statusBar->showMessage(
                    tr("Layer \"%1\" is locked - unlock it before cropping").arg(l->name()), 3500);
            return;
        }
    }

    const QRect canvas = m_doc->canvasRect();
    const QRect target = rect.intersected(canvas);

    if (target.width() < 1 || target.height() < 1) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The crop area is empty"), 3000);
        return;
    }

    // 框就是整幅画布 → Document::crop() 会返回 false。
    // 在这里提前挡住，免得在历史面板里留一条"什么都没改"的记录。
    if (target == canvas) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The crop area is the whole canvas - nothing to crop"), 3000);
        return;
    }

    // ★ 快照必须在修改之前 ★
    const GeometryChangeCommand::Snapshot before =
        GeometryChangeCommand::captureState(m_doc);

    if (!m_doc->crop(target)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Crop failed"), 3000);
        return;
    }

    commitGeometryChange(before, GeometryChangeCommand::Kind::Crop, tr("Crop"));
}

void MainWindow::cropToSelection()
{
    if (!requireDocument())
        return;

    if (!m_doc->selection().isActive()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Select the area to keep with a selection tool first"), 3000);
        return;
    }

    // 选区的包围盒可能带羽化边缘 —— bounds() 给的是"蒙版非零像素的范围"，
    // 正是"用户划到的地方"，不需要额外扩张。
    applyCropFromTool(m_doc->selection().bounds());
}

// ============================================================================
//  D. 图层操作
// ============================================================================
//
//  ★ 所有图层操作的统一骨架 ★
//
//    ① 记录 beforeLayers（cloneAll，浅拷贝，开销极低）
//    ② 调用 Document 的原始操作
//    ③ 记录 afterLayers
//    ④ 构造 LayerCommand 并 push
//
//  ★ "完整列表替换"的撤销策略 ★
//
//    LayerCommand 存的是"操作前/后的完整图层列表"，
//    因为 Layer 之间共享 QImage 缓冲区（QImage 赋值是浅拷贝），
//    所以存一整个列表的实际成本只有几十字节的元数据。
//    用"存完整列表"换"不用为每种操作写正向/反向逻辑"，非常划算。
//
//  ★ 但要注意"最后两个图层"的边界 ★
//    Document 必须始终有至少一个图层（否则画笔不知道往哪画）。
//    所以"删除图层"在只剩一个图层时要拒绝。

void MainWindow::addNewLayer()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    // ★ 在活动图层**上方**新增 ★
    //   Document::addLayer(name) 的语义就是这样（它内部把索引设为 active+1）。
    //   用户在一个图层上工作时点"新建图层"，期望新图层叠在它上面。
    const int newIndex = m_doc->addLayer();

    if (newIndex < 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to create a layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Add,
                                 beforeLayers, afterLayers,
                                 activeBefore, newIndex,
                                 tr("New Layer"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Layer created"), 2000);
}

void MainWindow::duplicateCurrentLayer()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int index = m_doc->activeLayerIndex();

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = index;

    if (!m_doc->duplicateLayer(index)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to duplicate the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Duplicate,
                                 beforeLayers, afterLayers,
                                 activeBefore, index + 1,   // 副本在原始之上
                                 tr("Duplicate Layer"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Layer duplicated"), 2000);
}

void MainWindow::deleteCurrentLayer()
{
    if (!requireDocument() || !m_commandStack)
        return;

    // ★ 最后一个图层的保护 ★
    //
    //  Document 必须始终有至少一个图层。删掉最后一个会让
    //  "活动图层"变成空指针，之后画笔、滤镜全部崩溃。
    //  这里明确拒绝，并告诉用户该怎么做。
    if (m_doc->layerCount() <= 1) {
        QMessageBox::information(this, tr("Cannot Delete the Layer"),
            tr("A document must keep at least one layer.\n\n"
               "To clear this layer's content, use \"Edit → Delete\"."));
        return;
    }

    const int index = m_doc->activeLayerIndex();

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = index;

    if (!m_doc->removeLayer(index)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to delete the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    // 删除后的活动图层：删除位置及其后一位都会被"上移"，
    // 所以新的活动索引应该钳制到有效范围。
    const int activeAfter = qBound(0, index, m_doc->layerCount() - 1);

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Remove,
                                 beforeLayers, afterLayers,
                                 activeBefore, activeAfter,
                                 tr("Delete Layer"));
    m_commandStack->push(cmd);

    // ★ 显式设置活动图层 ★
    //   Document::removeLayer 内部会调整 m_activeLayerIndex，
    //   但不同实现的调整策略可能不同。这里显式设一次，
    //   保证 UI 上的选中项与实际的活动图层一致。
    m_doc->setActiveLayerIndex(activeAfter);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Layer deleted"), 2000);
}

void MainWindow::moveLayerUp()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int from = m_doc->activeLayerIndex();

    // ★ "上移"意味着"索引 +1" ★
    //   Document 里索引 0 是最底部图层，所以"往上"是索引增大。
    //   这是最容易搞反的地方 —— 界面上的"↑"按钮对应索引 +1。
    const int to = from + 1;

    if (to >= m_doc->layerCount()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Already the topmost layer"), 2000);
        return;
    }

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();

    if (!m_doc->moveLayer(from, to)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to move the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Move,
                                 beforeLayers, afterLayers,
                                 from, to,
                                 tr("Move Layer Up"));
    m_commandStack->push(cmd);
}

void MainWindow::moveLayerDown()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int from = m_doc->activeLayerIndex();
    const int to = from - 1;   // 下移 = 索引减小

    if (to < 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Already the bottommost layer"), 2000);
        return;
    }

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();

    if (!m_doc->moveLayer(from, to)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to move the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Move,
                                 beforeLayers, afterLayers,
                                 from, to,
                                 tr("Move Layer Down"));
    m_commandStack->push(cmd);
}

void MainWindow::mergeLayerDown()
{
    // 菜单 / 快捷键：作用对象是活动图层。
    // 实现在 mergeLayerDownAt()，图层面板每一行的"向下合并"走同一个函数 ——
    // 合并的规则因此只有一份，不会出现"面板里能合、菜单里不能合"这种差异。
    //
    // （注意不要在这里写 requireDocument() + m_commandStack 的双重判断：
    //   真正的守卫在下面对偶函数里，写两遍只会让"改一处忘一处"。）
    if (!requireDocument())
        return;

    mergeLayerDownAt(m_doc->activeLayerIndex());
}

void MainWindow::mergeLayerDownAt(int layerIndex)
{
    if (!requireDocument() || !m_commandStack)
        return;

    if (m_doc->layerCount() < 2) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("At least two layers are required to merge"), 2500);
        return;
    }

    // ★ 上下界都要判 ★
    //   面板发来的索引可能已经过期（文档刚被撤销过一步、而面板的行还没重建）。
    //   只判下界的话，越界索引会一路走到 Document 被拒绝，
    //   用户看到的是"点了完全没反应"，而不是一句明确的提示。
    //
    //   下界为 1 而不是 0：索引 0 是最**底**的图层，它下面没有东西可合。
    const int index = layerIndex;
    if (index <= 0 || index >= m_doc->layerCount()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The bottommost layer cannot be merged down"), 2500);
        return;
    }

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = index;

    if (!m_doc->mergeLayerDown(index)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Merge down failed"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::MergeDown,
                                 beforeLayers, afterLayers,
                                 activeBefore, index - 1,   // 合并结果在下方图层的位置
                                 tr("Merge Down"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Merged down"), 2000);
}

void MainWindow::flattenImage()
{
    if (!requireDocument() || !m_commandStack)
        return;

    if (m_doc->layerCount() < 2) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Only one layer - nothing to flatten"), 2000);
        return;
    }

    // ★ 拼合不可逆 —— 先确认 ★
    //
    //  虽然本操作可以撤销，但拼合会**丢弃不可见图层的内容**，
    //  而用户可能没注意到某些图层是隐藏的。明确提示一下。
    int hiddenCount = 0;
    for (int i = 0; i < m_doc->layerCount(); ++i) {
        const Layer *layer = m_doc->layers().layer(i);
        if (layer && !layer->isVisible())
            ++hiddenCount;
    }

    QString message = tr("Flatten Image merges all visible layers into one.");
    if (hiddenCount > 0) {
        message += tr("\n\nNote: %1 hidden layers exist and their content will be discarded.").arg(hiddenCount);
    }
    message += tr("\n\nContinue?");

    if (QMessageBox::question(this, tr("Flatten Image"), message,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    if (!m_doc->flattenImage()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Flatten failed"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Flatten,
                                 beforeLayers, afterLayers,
                                 activeBefore, 0,
                                 tr("Flatten Image"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Image flattened"), 2000);
}

// ────────────────────────────────────────────────────────────────────────────
//  D2. 来自图层面板的操作（阶段五）
// ────────────────────────────────────────────────────────────────────────────
//
//  ★ 为什么"面板版本"要和"菜单版本"分开 ★
//
//    菜单 / 快捷键作用于**活动图层** —— 这时"活动图层"就是用户的意图。
//    面板上的操作却作用于**面板选中的若干行**：ExtendedSelection 下
//    用户可以框选三行，其中甚至不含活动图层。
//
//    如果让面板先把活动图层改成选中项、再调菜单版本，会有两个后果：
//      · 用户的选择状态被顺手改掉（点一下"删除"把活动图层也换了）
//      · 多选场景根本表达不出来（菜单版本只动一个图层）
//
//    所以面板走这一组"指定索引"的入口，菜单走原来那组。
//    两条路径最后都汇到 LayerCommand 压栈，撤销栈里看不出区别。

void MainWindow::deleteLayersFromPanel(const QList<int> &layerIndices)
{
    if (!requireDocument() || !m_commandStack)
        return;

    // 空列表 = 面板没选中任何行（正常情况下不会发生，面板会兜底选中
    // 活动图层）。退回"删除活动图层"比什么都不做更符合直觉 ——
    // 用户点了按钮却毫无反应才是最糟的。
    if (layerIndices.isEmpty()) {
        deleteCurrentLayer();
        return;
    }

    // ★ 先过滤 + 去重 + 降序，再动手 ★
    //
    //  降序不是为了好看：Document::removeLayer 会把后面的图层整体前移。
    //  按升序删的话，删掉索引 2 之后原来索引 4 的图层就变成 3 了，
    //  接下来那句 removeLayer(4) 删掉的是**另一个**图层（或者直接越界失败）。
    //  降序删就没有这个问题 —— 先删大的，小索引全程不受影响。
    QList<int> targets;
    for (int index : layerIndices) {
        if (index >= 0 && index < m_doc->layerCount() && !targets.contains(index))
            targets.append(index);
    }
    if (targets.isEmpty())
        return;
    std::sort(targets.begin(), targets.end(), std::greater<int>());

    // ★ 至少留一个图层 ★
    //   一次删光会让"活动图层"变成空指针 —— 之后画笔、滤镜全部崩溃。
    //   这里拒绝，并给出与菜单版本**完全相同**的提示文案
    //   （同一件事不该有两种说法，否则用户会以为是两个不同的限制）。
    if (targets.count() >= m_doc->layerCount()) {
        QMessageBox::information(this, tr("Cannot Delete the Layer"),
            tr("A document must keep at least one layer.\n\n"
               "To clear this layer's content, use \"Edit → Delete\"."));
        return;
    }

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    for (int index : targets)
        m_doc->removeLayer(index);

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    // 新的活动图层：被删掉的图层里比活动图层低的有几个，活动索引就往前挪几位。
    // 活动图层自己也被删掉时，同一个式子给出的正是"顶上来接替它的那一个"；
    // 删到末尾的情况靠 qBound 兜底。
    int removedBelow = 0;
    for (int index : targets) {
        if (index < activeBefore)
            ++removedBelow;
    }
    const int activeAfter = qBound(0, activeBefore - removedBelow,
                                   m_doc->layerCount() - 1);

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Remove,
                                 beforeLayers, afterLayers,
                                 activeBefore, activeAfter,
                                 targets.count() > 1 ? tr("Delete Multiple Layers")
                                                     : tr("Delete Layer"));
    m_commandStack->push(cmd);

    // ★ 显式设置活动图层 ★
    //   Document::removeLayer 内部虽然也会调整 m_activeLayerIndex，
    //   但"怎么调"是它自己的实现细节。这里显式设一次，保证面板的选中项
    //   与实际活动图层一致（与 deleteCurrentLayer 同款）。
    m_doc->setActiveLayerIndex(activeAfter);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Deleted %1 layers").arg(targets.count()), 2000);
}

void MainWindow::duplicateLayersFromPanel(const QList<int> &layerIndices)
{
    if (!requireDocument() || !m_commandStack)
        return;

    if (layerIndices.isEmpty()) {
        duplicateCurrentLayer();
        return;
    }

    // ★ 复制要**升序**处理，并且每插一个副本就要校正一次索引 ★
    //   每次 duplicateLayer(i) 都会在 i 上方插入一个副本，
    //   于是后面所有源图层的索引整体 +1。处理第 k 个源图层时，
    //   它实际已经挪到 index + k 的位置了。
    //   忘了校正的表现是"框选三行复制，复制出来的是别的图层" ——
    //   数量对得上，内容对不上，很难一眼看穿。
    QList<int> targets;
    for (int index : layerIndices) {
        if (index >= 0 && index < m_doc->layerCount() && !targets.contains(index))
            targets.append(index);
    }
    if (targets.isEmpty())
        return;
    std::sort(targets.begin(), targets.end());

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    int inserted = 0;
    for (int index : targets) {
        if (m_doc->duplicateLayer(index + inserted))
            ++inserted;
    }

    if (inserted == 0) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to duplicate the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    // 活动图层：Document::duplicateLayer 每次都会把活动索引设成新副本，
    // 所以循环结束后它正指着"最后一个副本"。这里仍显式跟一次 ——
    // 免得将来 Document 改了行为而这里悄悄失去同步。
    const int activeAfter = qBound(0, m_doc->activeLayerIndex(),
                                   m_doc->layerCount() - 1);

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Duplicate,
                                 beforeLayers, afterLayers,
                                 activeBefore, activeAfter,
                                 inserted > 1 ? tr("Duplicate Multiple Layers")
                                              : tr("Duplicate Layer"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Duplicated %1 layers").arg(inserted), 2000);
}

void MainWindow::reorderLayer(int fromIndex, int toIndex)
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int count = m_doc->layerCount();
    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count)
        return;

    // ★ 原地放回：不要压一条"什么都没变"的命令 ★
    //   拖拽时"拖起来又放回原处"非常常见。压进去的话，用户按 Ctrl+Z
    //   会看到画面毫无变化，于是以为撤销坏了 —— 那比不能撤销更糟。
    if (fromIndex == toIndex)
        return;

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();

    if (!m_doc->moveLayer(fromIndex, toIndex)) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("Failed to move the layer"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::Move,
                                 beforeLayers, afterLayers,
                                 fromIndex, toIndex,
                                 tr("Reorder Layers"));
    m_commandStack->push(cmd);
}

void MainWindow::mergeVisibleLayers()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const QList<Layer> beforeLayers = m_doc->layers().cloneAll();
    const int activeBefore = m_doc->activeLayerIndex();

    if (!m_doc->mergeVisibleLayers()) {
        // 可见图层不足两个时 Document 返回 false。这不是"出错"，
        // 而是一种有意义的现状 —— 所以给可执行的提示，而不是"失败"。
        if (m_statusBar)
            m_statusBar->showMessage(tr("At least two visible layers are required to merge"), 2500);
        return;
    }

    const QList<Layer> afterLayers = m_doc->layers().cloneAll();
    const int activeAfter = qBound(0, m_doc->activeLayerIndex(),
                                   m_doc->layerCount() - 1);

    auto *cmd = new LayerCommand(m_doc, LayerCommand::Op::MergeVisible,
                                 beforeLayers, afterLayers,
                                 activeBefore, activeAfter,
                                 tr("Merge Visible Layers"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Visible layers merged"), 2000);
}

// ── 图层蒙版 ──
//
//  MVP 只做"用选区生成"与"删除"。不做画笔涂蒙版：那需要工具层知道
//  "现在写的是蒙版而不是图层像素"，是另一个量级的改动。
//  即便如此，非破坏性遮挡这个核心价值已经成立 —— 而且随时可撤销、可删除。

void MainWindow::addLayerMaskFromSelection()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int index = m_doc->activeLayerIndex();
    Layer *layer = m_doc->layers().layer(index);
    if (!layer)
        return;

    if (layer->isLocked()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is not editable"), 2500);
        return;
    }

    const Selection &selection = m_doc->selection();
    if (!selection.isActive() || selection.mask().isNull()) {
        // 没有选区就做全白蒙版是"看得见但没用"，而且本版还不能涂蒙版，
        // 所以直接说清楚要什么，而不是塞一个空壳给用户。
        if (m_statusBar)
            m_statusBar->showMessage(
                tr("Select an area first; that area becomes the visible part"), 5000);
        return;
    }

    auto *cmd = LayerCommand::createPropertyCommand(
        m_doc, index, LayerCommand::Op::SetMask,
        QVariant::fromValue(layer->mask()),
        QVariant::fromValue(selection.mask()),
        tr("Add Layer Mask"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Layer mask added"), 3000);
}

void MainWindow::removeLayerMask()
{
    if (!requireDocument() || !m_commandStack)
        return;

    const int index = m_doc->activeLayerIndex();
    Layer *layer = m_doc->layers().layer(index);
    if (!layer)
        return;

    if (!layer->hasMask()) {
        if (m_statusBar)
            m_statusBar->showMessage(tr("This layer has no mask"), 3000);
        return;
    }

    auto *cmd = LayerCommand::createPropertyCommand(
        m_doc, index, LayerCommand::Op::SetMask,
        QVariant::fromValue(layer->mask()),
        QVariant::fromValue(QImage()),
        tr("Remove Layer Mask"));
    m_commandStack->push(cmd);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Layer mask removed"), 3000);
}

void MainWindow::renameActiveLayer()
{
    if (!requireDocument() || !m_layerPanel)
        return;

    // 面板负责"把哪一行变成编辑框"（行序换算在面板内部）；
    // 这里只转达意图，所以菜单项与 F2 不会各自算错行号。
    m_layerPanel->beginRenameActiveLayer();
}

// ── 图层属性（眼睛 / 锁 / 名字 / 混合模式 / 不透明度）──
//
//  ★ 这一组全部走 LayerCommand::createPropertyCommand ★
//    属性类命令只存"一个图层的某个属性"的前后值，不需要整表快照 ——
//    比结构类命令省得多，而且撤销时不会碰到别的图层。

void MainWindow::setLayerVisible(int layerIndex, bool visible)
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    // 值没变就不压栈。双击眼睛、或"点了两次同一个眼睛"会走到这里；
    // 压进去的话撤销栈里会出现一堆"看不见变化"的项。
    if (layer->isVisible() == visible)
        return;

    auto *cmd = LayerCommand::createPropertyCommand(
                m_doc, layerIndex, LayerCommand::Op::SetVisible,
                QVariant(layer->isVisible()), QVariant(visible),
                visible ? tr("Show Layer") : tr("Hide Layer"));
    m_commandStack->push(cmd);
}

void MainWindow::setLayerLocked(int layerIndex, bool locked)
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    if (layer->isLocked() == locked)
        return;

    auto *cmd = LayerCommand::createPropertyCommand(
                m_doc, layerIndex, LayerCommand::Op::SetLocked,
                QVariant(layer->isLocked()), QVariant(locked),
                locked ? tr("Lock Layer") : tr("Unlock Layer"));
    m_commandStack->push(cmd);
}

void MainWindow::setLayerName(int layerIndex, const QString &name)
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    // ★ 去掉首尾空白，并拒绝空名字 ★
    //   不然用户误按回车（或只敲了空格）会把图层名清成空字符串，
    //   列表里那一行看起来就像"少了一个图层"。
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == layer->name())
        return;

    auto *cmd = LayerCommand::createPropertyCommand(
                m_doc, layerIndex, LayerCommand::Op::Rename,
                QVariant(layer->name()), QVariant(trimmed),
                tr("Rename Layer"));
    m_commandStack->push(cmd);
}

void MainWindow::setLayerBlendMode(int layerIndex, int blendBefore, int blendAfter)
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    // ★ 以文档里的实际值为 before，不信任传进来的那个 ★
    //   面板发来的 before 是"它自己以为的当前值"，而面板的回填可能因为
    //   m_syncing 抑制而被跳过。以文档为准，撤销才能回到真正的前一个值。
    const int before = int(layer->blendMode());
    if (before == blendAfter)
        return;

    auto *cmd = LayerCommand::createPropertyCommand(
                m_doc, layerIndex, LayerCommand::Op::SetBlendMode,
                QVariant(before), QVariant(blendAfter),
                tr("Change Blend Mode"));
    m_commandStack->push(cmd);

    Q_UNUSED(blendBefore);
}

void MainWindow::previewLayerOpacity(int layerIndex, int opacity)
{
    if (!requireDocument())
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    const quint8 value = quint8(qBound(0, opacity, 255));
    if (layer->opacity() == value)
        return;

    // ★ 拖动中只改内存、不压栈，也不发 structureChanged ★
    //
    //  只改内存：拖一次滑块会发几十次 valueChanged，每次都压一条命令的话，
    //    用户要按几十次 Ctrl+Z 才能退回去 —— 撤销栈被污染最典型的形态。
    //    松手时 commitLayerOpacity() 会补上**一条**命令，把整段拖动当成一次操作。
    //
    //  不发 structureChanged：那条信号会让面板整表重建 + 回填控件，
    //    而此刻用户正按着滑块不放，重建既浪费又可能和拖动打架。
    //    markDirty() 已经足够 —— 它让合成缓存失效，画布立刻重画。
    layer->setOpacity(value);
    m_doc->markDirty();
}

void MainWindow::commitLayerOpacity(int layerIndex, int before, int after)
{
    if (!requireDocument() || !m_commandStack)
        return;

    Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    Q_UNUSED(layer);

    const int from = qBound(0, before, 255);
    const int to   = qBound(0, after, 255);
    if (from == to)
        return;

    // ★ 这里不需要"把内存恢复到 from" ★
    //   拖动路径下内存里已经是 to 了，而 QUndoStack::push() 会立刻调用
    //   redo()，把 to 再写一次 —— 幂等。撤销时 undo() 写回 from，
    //   正是拖动开始前的值，也就是说"一次 Ctrl+Z 撤销整段拖动"。
    //   键盘路径（方向键/点轨道）下内存还是 from，redo() 才真正生效。
    //   两条路径共用这一句，不需要分支。
    auto *cmd = LayerCommand::createPropertyCommand(
                m_doc, layerIndex, LayerCommand::Op::SetOpacity,
                QVariant(from), QVariant(to),
                tr("Change Opacity"));
    m_commandStack->push(cmd);
}

// ============================================================================
//  视图操作
// ============================================================================

void MainWindow::toggleFullScreen()
{
    if (!isFullScreen()) {
        // 记住进来之前是不是最大化，退出时要还原成那个状态。
        m_wasMaximizedBeforeFullScreen = isMaximized();
        showFullScreen();
    } else if (m_wasMaximizedBeforeFullScreen) {
        showMaximized();
    } else {
        showNormal();
    }
}

void MainWindow::zoomIn()
{
    if (m_canvas)
        m_canvas->zoomIn();
}

void MainWindow::zoomOut()
{
    if (m_canvas)
        m_canvas->zoomOut();
}

void MainWindow::zoomToFit()
{
    if (m_canvas)
        m_canvas->zoomToFit();
}

void MainWindow::zoomToActualSize()
{
    if (m_canvas)
        m_canvas->zoomToActualSize();
}

void MainWindow::setZoomLevel(qreal level)
{
    if (m_canvas)
        m_canvas->setZoomLevel(level);
}

void MainWindow::toggleGrid(bool visible)
{
    if (m_canvas)
        m_canvas->setGridVisible(visible);
}

// ============================================================================
//  网格的间距与吸附（阶段七收尾）
// ============================================================================

void MainWindow::toggleSnapToGrid(bool enabled)
{
    // 与「显示网格」一样是**应用级偏好**：只记在 MainWindow 的记忆里，
    // 由 applyDocumentToPanels() 在切页/新页时补推给每一块画布
    // （画布是每页一份，属性不补推就不会跟着走）。
    if (m_canvas)
        m_canvas->setSnapToGrid(enabled);

    if (m_statusBar)
        m_statusBar->showMessage(enabled ? tr("Snap to grid on")
                                         : tr("Snap to grid off"));
}

void MainWindow::setGridSpacing(int pixels)
{
    // ★ 先更新单一来源，再推给画布 ★
    //   反过来的话，"切页补推"会用到还没更新的旧值。
    m_gridSpacing = qBound(4, pixels, 2000);

    if (m_canvas)
        m_canvas->setGridSize(m_gridSpacing);

    // 菜单勾选态与值同步（自定义值不在预设里时 6 项全不勾）
    for (int i = 0; i < m_actionGridSpacing.size(); ++i)
        m_actionGridSpacing.at(i)->setChecked(
            m_actionGridSpacing.at(i)->data().toInt() == m_gridSpacing);

    // 只改间距不必开网格 —— 与「显示网格」独立。状态栏提示让用户知道生效了
    // （否则在没开网格时改间距看起来毫无反应）。
    if (m_statusBar)
        m_statusBar->showMessage(tr("Grid spacing: %1 px").arg(m_gridSpacing));
}

void MainWindow::editGridSettings()
{
    if (!m_canvas)
        return;

    // ★ 模态对话框在无人环境会永久挂死 ★
    //   与本工程其它对话框（图像大小/画布大小…）同一条纪律：
    //   "干活的那半"必须是可被程序驱动的公开入口 —— 这里就是
    //   setGridSpacing() 与画布的 setGridColor()。对话框只负责收集意愿，
    //   真正的改动由那两个入口完成，自检因此可以绕开 exec() 直接验行为。
    GridSettingsDialog dlg(m_gridSpacing, m_gridColor, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // 走与预设项同一条路（顺带同步勾选态与状态栏提示）
    setGridSpacing(dlg.spacing());

    if (dlg.color().isValid() && dlg.color() != m_gridColor) {
        m_gridColor = dlg.color();
        if (m_canvas)
            m_canvas->setGridColor(m_gridColor);
    }
}

void MainWindow::toggleCheckerboard(bool visible)
{
    if (m_canvas)
        m_canvas->setCheckerboardVisible(visible);
}

// ============================================================================
//  视图辅助：标尺与参考线（阶段七）
// ============================================================================

void MainWindow::toggleRuler(bool visible)
{
    // ★ 三件一起管 ★
    //   拐角不跟着走的话，尺子藏了它还留在左上角，就是一个孤零零的灰方块
    //   （在深色背景上看起来像渲染错误）。
    if (m_hruler)
        m_hruler->setVisible(visible);
    if (m_vruler)
        m_vruler->setVisible(visible);
    if (m_rulerCorner)
        m_rulerCorner->setVisible(visible);
}

void MainWindow::setRulerUnit(int index)
{
    // ★ 越界必须先挡掉 ★
    //   static_cast 到枚举类型时越界值不会报错，而是让 setUnit() 拿到一个
    //   谁也认不出的枚举 —— 换算全都走 default 分支，表现是"单位点了没反应"。
    //   挡在这里比在下游到处兜底便宜得多。
    if (index < 0 || index >= rulermetrics::kUnitCount)
        return;

    const rulermetrics::Unit unit = rulermetrics::unitAt(index);
    if (m_hruler)
        m_hruler->setUnit(unit);
    if (m_vruler)
        m_vruler->setUnit(unit);
}

void MainWindow::toggleGuides(bool visible)
{
    if (m_canvas)
        m_canvas->setGuidesVisible(visible);
}

void MainWindow::toggleGuidesLocked(bool locked)
{
    if (m_canvas)
        m_canvas->setGuidesLocked(locked);
}

void MainWindow::clearGuides()
{
    if (!m_canvas)
        return;
    m_canvas->clearGuides();
    if (m_statusBar)
        m_statusBar->showMessage(tr("Cleared the guides on this page"));
}

// ============================================================================
//  颜色操作
// ============================================================================

void MainWindow::setForegroundColor(const QColor &c)
{
    if (!c.isValid() || c == m_foregroundColor)
        return;

    m_foregroundColor = c;

    // 同步到画布（画布把这个颜色提供给工具使用）
    if (m_canvas)
        m_canvas->setForegroundColor(c);

    // 同步所有颜色显示
    updateColorIndicators();
}

void MainWindow::setBackgroundColor(const QColor &c)
{
    if (!c.isValid() || c == m_backgroundColor)
        return;

    m_backgroundColor = c;

    if (m_canvas)
        m_canvas->setBackgroundColor(c);

    updateColorIndicators();
}

void MainWindow::chooseForegroundColor()
{
    // ★ 用 QColorDialog::getColor 的静态版本 ★
    //   它内部会创建一个模态对话框并在关闭时自动销毁 ——
    //   不需要手动 new/delete，也不会泄漏。
    //
    //   第二个参数传 this 让它居中于主窗口。
    //   QColorDialog::ShowAlphaChannel 让用户能选半透明色 ——
    //   这对图像编辑很重要（画笔要能画半透明的颜色）。
    const QColor color = QColorDialog::getColor(
        m_foregroundColor, this, tr("Choose Foreground Color"),
        QColorDialog::ShowAlphaChannel);

    if (color.isValid())
        setForegroundColor(color);
}

void MainWindow::chooseBackgroundColor()
{
    const QColor color = QColorDialog::getColor(
        m_backgroundColor, this, tr("Choose Background Color"),
        QColorDialog::ShowAlphaChannel);

    if (color.isValid())
        setBackgroundColor(color);
}

void MainWindow::swapColors()
{
    // ★ 用 std::swap 而不是临时变量手动交换 ★
    //   手动写法容易漏掉一行（"忘了交换背景色"），
    //   而且对 QColor 这种有赋值开销的类，std::swap 会走移动语义。
    std::swap(m_foregroundColor, m_backgroundColor);

    if (m_canvas) {
        m_canvas->setForegroundColor(m_foregroundColor);
        m_canvas->setBackgroundColor(m_backgroundColor);
    }

    updateColorIndicators();

    if (m_statusBar)
        m_statusBar->showMessage(tr("Swapped the foreground and background colors"), 1500);
}

void MainWindow::resetColors()
{
    // ★ 默认颜色是黑/白 ★
    //   不用"透明/白"：画笔的默认颜色必须是可见的，
    //   而"透明的前景色"会让用户以为画笔坏了。
    //   需要透明的画笔时，用户可以自己把 alpha 调到 0。
    setForegroundColor(Qt::black);
    setBackgroundColor(Qt::white);

    if (m_statusBar)
        m_statusBar->showMessage(tr("Restored the default colors (black / white)"), 1500);
}

// ============================================================================
//  E. 参数化调整（亮度/对比度、色相/饱和度、曲线）
// ============================================================================
//
//  这一类操作与 C 节的"去色/反相"的区别只有一个：需要先收集参数。
//  但正是这一个区别带来了三个新问题，本节的代码就是为它们而写：
//
//    ① 用户要在拖动时**实时看到结果** → 降采样预览（见 runAdjustment）
//    ② 拖动过程会产生几十次中间状态 → 必须只压**一条**撤销记录
//    ③ 参数没动过就点确定 → 不能压栈（否则历史面板被空记录灌满）
//
//  ★ 一个必须守住的约定：预览所见 == 确定所得 ★
//
//    做法是把"参数 → 图像"这件事收敛成对话框的一个纯函数 apply()，
//    预览和最终提交都调它，区别只在输入图的大小（1024 vs 全尺寸）。
//    任何"预览走一条路径、提交走另一条路径"的写法迟早会分叉。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  取调整操作的源图
// ---------------------------------------------------------------------------
QImage MainWindow::prepareAdjustmentSource()
{
    if (!requireDocument() || !m_commandStack)
        return QImage();

    Layer *layer = m_doc->activeLayer();
    if (!layer || layer->isLocked()) {
        // 锁定图层不允许改像素。这里必须给出提示而不是静默返回 ——
        // 静默返回会让用户以为"这个菜单项坏了"。
        if (m_statusBar)
            m_statusBar->showMessage(tr("The current layer is not editable"), 2500);
        return QImage();
    }

    return captureActiveLayerImage();
}

// ---------------------------------------------------------------------------
//  三个入口（结构与"去色/反相"一致，只是多了一步对话框）
// ---------------------------------------------------------------------------
void MainWindow::openBrightnessContrast()
{
    // ★ 先取源图，再把它交给对话框构造 ★
    //   对话框需要源图统计背景直方图，runAdjustment 需要同一张图作为
    //   "before"。共用一份可以省掉一次整层拷贝（4000×3000 就是 48MB）。
    const QImage base = prepareAdjustmentSource();
    if (base.isNull())
        return;

    runAdjustment(new BrightnessContrastDialog(base, this), base, tr("Brightness/Contrast"));
}

void MainWindow::openHueSaturation()
{
    const QImage base = prepareAdjustmentSource();
    if (base.isNull())
        return;

    runAdjustment(new HueSaturationDialog(base, this), base, tr("Hue/Saturation"));
}

void MainWindow::openCurves()
{
    const QImage base = prepareAdjustmentSource();
    if (base.isNull())
        return;

    // 曲线对话框的构造里会算一次亮度直方图（约 30ms，1200 万像素）。
    // 放在构造里而不是每帧，是因为直方图完全不变 —— 它在对话框整个
    // 生命周期内是常量。
    runAdjustment(new CurvesDialog(base, this), base, tr("Curves"));
}

void MainWindow::openLevels()
{
    const QImage base = prepareAdjustmentSource();
    if (base.isNull())
        return;

    // 与曲线同理：色阶也需要一份亮度直方图，同样在构造里算一次
    // （对整层遍历一遍约 30ms，放在对话框构造里而非每帧）。
    // 两者共用 histogram::luminance()，统计口径一致 ——
    // 否则用户会在曲线里看到的峰值位置与色阶对不上。
    runAdjustment(new LevelsDialog(base, this), base, tr("Levels"));
}

// ---------------------------------------------------------------------------
//  runAdjustment —— 打开对话框、预览、按结果压一条命令
// ---------------------------------------------------------------------------
bool MainWindow::runAdjustment(AdjustmentDialog *dialog, const QImage &base,
                               const QString &historyText)
{
    // ★ 用 QScopedPointer 接管所有权 ★
    //   本函数有多条 return 路径（基础检查失败、取消、参数没变…），
    //   裸指针 + 手工 delete 迟早会在某条后来新增的路径上漏掉，
    //   而漏掉的后果是每次打开这个对话框就泄漏一份 48MB 的位图。
    QScopedPointer<AdjustmentDialog> guard(dialog);

    if (!dialog || base.isNull() || !m_doc || !m_commandStack || !m_canvas)
        return false;

    // ── 准备预览状态 ──
    m_adjustBase   = base;
    m_adjustText   = historyText;
    m_adjustDialog = dialog;

    const QSize sz = base.size();
    const int longest = qMax(sz.width(), sz.height());

    if (longest > kAdjustPreviewMaxSide) {
        // 按最长边等比缩到 kAdjustPreviewMaxSide。
        // 用 Smooth 是刻意的：预览基准图会被放大回画布尺寸呈现，
        // 若它本身带锯齿，放大会变成马赛克，用户会误以为调整把画面弄脏了。
        const QSize previewSize(qRound(sz.width()  * qreal(kAdjustPreviewMaxSide) / longest),
                                qRound(sz.height() * qreal(kAdjustPreviewMaxSide) / longest));
        m_adjustPreviewBase = base.scaled(previewSize, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation);
    } else {
        // 小图不降采样：预览就是最终质量，也不必付缩放的代价
        m_adjustPreviewBase = base;
    }

    // ── 预先缩放选区蒙版 ──
    //
    // 对话框是模态的，期间选区不可能变化 —— 所以缩放只做一次。
    // 放进每帧的预览路径会白白多花几毫秒（缩放 100 万像素约 5ms）。
    m_adjustPreviewMask = QImage();
    m_adjustPreviewMaskBounds = QRect();

    const Selection &selection = m_doc->selection();
    if (selection.isActive() && !selection.mask().isNull()) {
        const QImage &fullMask = selection.mask();
        m_adjustPreviewMask = fullMask.scaled(m_adjustPreviewBase.size(),
                                              Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation);

        if (fullMask.width() > 0 && fullMask.height() > 0) {
            const qreal sx = qreal(m_adjustPreviewMask.width())  / fullMask.width();
            const qreal sy = qreal(m_adjustPreviewMask.height()) / fullMask.height();
            const QRect b = selection.bounds();

            // ★ 向外扩 2 像素 ★
            //   等比缩小后，选区边界会落在像素中间，向下取整会把最外
            //   一两行切掉 —— 表现为选区边缘出现一条生硬的直边，
            //   羽化看起来"断"了。多扫两行完全不影响正确性（蒙版值是 0）。
            m_adjustPreviewMaskBounds =
                QRect(qFloor(b.x() * sx) - 2, qFloor(b.y() * sy) - 2,
                      qCeil(b.width() * sx) + 4, qCeil(b.height() * sy) + 4);
        }
    }

    connect(dialog, &AdjustmentDialog::previewNeeded,
            this, &MainWindow::updateAdjustmentPreview);

    // 建立初始预览：此时参数都是初值，画面看起来和原来一样 ——
    // 但走一遍能提前确认"预览通道是通的"，而不是等用户拖了滑块
    // 才发现毫无反应（那种无声的失效最容易让人以为功能没做）。
    updateAdjustmentPreview();

    const int result = dialog->exec();

    // ★ 先断信号再清状态 ★
    //   对话框析构时其子控件仍可能发出 valueChanged。若那时
    //   m_adjustDialog 已被清空，槽函数里的空判断虽然能挡住，
    //   但更稳妥的做法是从源头断开连接，避免任何"析构期间回调"
    //   带来的时序不确定性。
    disconnect(dialog, nullptr, this, nullptr);
    m_adjustDialog = nullptr;

    bool applied = false;

    if (result == QDialog::Accepted && dialog->isChanged()) {
        // ★ 顺序不能反：先清预览，再压命令 ★
        //   若先压命令，命令触发的重绘会被"预览图优先"的分支吃掉，
        //   画面停在一张降采样图上 —— 用户看到的是"点确定之后图变糊了"。
        m_canvas->clearPreviewImage();

        QApplication::setOverrideCursor(Qt::WaitCursor);
        QImage after = dialog->apply(m_adjustBase);   // 全尺寸运算
        QApplication::restoreOverrideCursor();

        if (after.isNull()) {
            if (m_statusBar)
                m_statusBar->showMessage(tr("Adjustment failed: no result generated"), 3000);
        } else {
            // 有选区时只处理选区内的像素（与去色/反相完全一致）
            if (m_doc->selection().isActive())
                after = blendWithSelection(m_adjustBase, after, m_doc->selection());

            pushActiveLayerImageCommand(m_adjustBase, after, m_adjustText);
            applied = true;

            if (m_statusBar)
                m_statusBar->showMessage(tr("Applied %1").arg(m_adjustText), 2500);
        }
    } else {
        // 取消 / 参数没变：文档从头到尾没被碰过，不需要"还原"，只需
        // 把画布切回文档的真实内容。这正是"预览不进 Document"的收益。
        m_canvas->clearPreviewImage();
    }

    // ── 清理 ──
    // 大图时这几张都是几十 MB，及时释放能显著降低长时间编辑的内存峰值。
    m_adjustBase = QImage();
    m_adjustPreviewBase = QImage();
    m_adjustPreviewMask = QImage();
    m_adjustPreviewMaskBounds = QRect();
    m_adjustText.clear();

    return applied;
}

// ---------------------------------------------------------------------------
//  updateAdjustmentPreview —— 重算降采样预览并交给画布
// ---------------------------------------------------------------------------
void MainWindow::updateAdjustmentPreview()
{
    if (!m_adjustDialog || !m_canvas)
        return;

    // 用户取消了"预览"勾选：把画布切回文档的真实内容。
    // 参数保持不动 —— 于是"关掉预览对比一下、再打开"是零成本的，
    // 这也是 Photoshop 里"预览"复选框的语义。
    if (!m_adjustDialog->isPreviewEnabled()) {
        m_canvas->clearPreviewImage();
        return;
    }

    // ★ 每一帧都从预览基准图重算 ★
    //   绝不在"上一次的预览结果"上继续变换：LUT 本身是确定性的，
    //   但每次运算都伴随一次"预乘 ⇄ 非预乘"的格式转换，舍入误差会
    //   随着拖动次数累积，最后整体偏色（见 docs/曲线调整设计.md 5.2）。
    const QImage rendered = m_adjustDialog->apply(m_adjustPreviewBase);
    if (rendered.isNull()) {
        m_canvas->clearPreviewImage();
        return;
    }

    QImage preview = rendered;

    // ★ 预览也必须按选区混合 ★
    //   否则会出现"预览里整幅图都变了，确定后只有选区内变"——
    //   与"预览所见 == 确定所得"的约定直接冲突。这也是把
    //   blendByMask 从 blendWithSelection 里拆出来的原因。
    if (!m_adjustPreviewMask.isNull()) {
        preview = blendByMask(m_adjustPreviewBase, rendered,
                              m_adjustPreviewMask, m_adjustPreviewMaskBounds);
    }

    m_canvas->setPreviewImage(preview);
}

} // namespace zl
