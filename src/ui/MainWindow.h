#pragma once

// ============================================================================
//  MainWindow.h —— 主窗口
// ----------------------------------------------------------------------------
//  职责：把所有部件组装起来，并实现"应用级"的操作（文件生命周期、菜单命令）。
//
//  ★ 内容相关的逻辑不放在这里 ★
//
//    这个类是**编排者**，不是实现者。具体的事情都由下层完成：
//      图像操作   → Engine 的纯函数 + Document 的方法
//      撤销       → commands/ 层的各类 Command
//      画布交互   → CanvasView + tools/
//      工具集合   → ToolManager
//
//    MainWindow 只负责：
//      · 建菜单/工具栏/状态栏/停靠面板
//      · 把信号连起来
//      · 实现"文件生命周期"（新建/打开/保存/另存为/关闭提示）
//      · 实现那些"需要跨模块协调"的命令（例如"复制"要同时读 Document
//        与 Selection，还要写系统剪贴板，属于跨模块协调）
//
//  ★ 遵守约定 7 ★
//    所有面板与工具都提供 setDocument()。切换文档时调用一次
//    applyDocumentToPanels() 就完成全部切换 —— 而不是重建控件。
//    这是多标签化（阶段八）能低成本实现的前提。
// ============================================================================

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QSize>
#include <QRect>
#include <QColor>
#include <QImage>
#include <QIcon>
#include <QHash>
#include <QList>
// ★ 必须包含 QAction 的完整定义（而不是前置声明）★
//   因为下面有 QList<QAction *> 成员与 QHash<QString, QAction *> 成员。
//   QList<T*> 的析构需要 T 是完整类型（虽然对指针其实不必要，
//   但 Qt5 的 QList 在部分配置下会实例化 sizeof(T)，导致不完整类型报错）。
#include <QAction>

// ★ 需要完整定义 ★
//   setScreenGrabber() 的形参是 std::function<QImage()> —— 值传递，必须完整类型。
#include <functional>

// ★ 需要完整定义，而不是前置声明 ★
//   captureFailureText() 的形参是 CaptureBlock —— 这是个 enum class，
//   前置声明它要么写成 `enum class CaptureBlock;`（需要固定底层类型才合规），
//   要么就得让每个包含 MainWindow.h 的文件都去猜它的底层类型。
//   直接把 compat 头文件引进来：它只依赖 QImage，代价极小。
#include "compat/ScreenCapture.h"

// ★ 需要完整定义，而不是前置声明 ★
//   commitGeometryChange() 的参数用到了 GeometryChangeCommand 的两个
//   **嵌套类型**（Snapshot / Kind），嵌套类型无法前置声明。
//   依赖方向 ui → commands 是合法的（见架构铁律第 1 条）。
#include "commands/GeometryChangeCommand.h"

class QLabel;
class QDockWidget;
class QToolBar;
class QUndoView;
class QComboBox;
class QPrinter;
// ★ Qt 的类只能声明在**全局**命名空间 ★
//   曾经把 `class QTimer;` 写进了下面的 namespace zl 里 —— 那声明出来的是
//   **zl::QTimer**，一个与 ::QTimer 毫不相干的类。头文件里（成员是指针）
//   看不出任何异常，报错要等到 .cpp 里去 new 它：
//   "使用了未定义类型 zl::QTimer"。同族的坑见下面 GlobalHotkey 的说明。
class QTimer;

namespace zl {

class LayerPanel;
class NavigatorPanel;

class Document;
class DocumentManager;
class RecoveryManager;
class CommandStack;
class Selection;
class AbstractTool;   // applyActiveToolToUi() 的形参（只用到指针）
struct ClipboardData;   // ★ 它是 struct（纯数据聚合），前置声明必须用 struct ★
                       //   用 class 声明会在 MSVC 下报 C4099 警告，
                       //   而且如果两种声明在同一个 TU 里出现，会升级成错误。
class CanvasView;
class RulerWidget;
class ToolManager;
class ToolOptionsBar;
class StatusBarWidget;
class GlobalHotkey;   // compat/GlobalHotkey.h（只用到指针，无需完整定义）
class AdjustmentDialog;
class RotateImageDialog;
struct DocumentTab;   // ★ 它是 struct（纯数据聚合），前置声明必须用 struct ★
                      //   用 class 声明会得到 C4099 警告，且两种写法在同一个
                      //   TU 里出现过就会升级成错误。与 ClipboardData 同源。
class DocumentTabs;

// ============================================================================
//  MainWindow
// ============================================================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // initialFiles —— 启动时要打开的文件（命令行参数）。为空则新建一个空白文档。
    //
    // ★ 为什么走构造参数，而不是"构造完再调一个 open" ★
    //   后者会先建一个空白页、再把文件开成新页，于是用户启动程序时
    //   会多出一个用不上的"未命名-1"标签，还得手动关掉。
    //   "无参数 → 空白文档；有参数 → 打开参数里的文件"是命令行程序的
    //   通行约定（`program a.png b.png` 不该额外送你一块空画布）。
    explicit MainWindow(QWidget *parent = nullptr,
                        const QStringList &initialFiles = QStringList());
    ~MainWindow() override;

    // ── 自动化入口（仅自检 / 截图工具使用）────────────────────────────────
    //
    // ★ 为什么必须把它从 openAsLayer() 里拆出来 ★
    //   openAsLayer() 的第一件事是 QFileDialog —— 模态对话框在无人的
    //   自动化环境里会让进程直接卡死（截图工具就会挂在这一行）。
    //   把"选文件"之后的全部逻辑做成一个吃路径的函数，于是同一条
    //   导入路径既能被人用（openAsLayer 选完文件后调它），也能被脚本
    //   驱动（shot 工具直接给路径）。对话框退化成"路径的来源之一"。
    //
    //   与 TextTool::placeText() 是同一个套路：把"有对话框"和
    //   "真正干活"分开，干活的那半必须能脱离 UI 被调用。
    //   本工程里凡是带对话框的功能都该长成这样 —— 否则它就永远
    //   只能靠人眼验证，而没有断言守着。
    //
    //   注意：本方法是 public 但不进菜单、不算"功能"。产品口径上
    //   用户能看到的入口仍然只有「文件 → 打开为图层…」一个。

    // 导入图比画布大时怎么办。
    enum class GrowPolicy {
        Ask,    // 弹窗询问（openAsLayer 用 —— 有人可问就必须问）
        Grow,   // 直接扩大画布以容纳（自动化用）
        Crop,   // 保持画布尺寸，超出部分裁掉（自动化用）
    };

    // 把 path 指向的图片整幅导入为**当前文档**的一个新图层。
    //
    // 返回值：true = 成功压入了撤销记录；false = 失败或取消
    //         （读不出元数据、加载失败、图层锁定、policy == Ask 时用户取消）。
    //
    // ★ 撤销粒度在这里定死为一条 ★
    //   内部自己 beginMacro/endMacro，把"扩画布（可选）+ 加图层"合成一条，
    //   用户按一次 Ctrl+Z 回到导入之前。失败路径上也会 endMacro —— 
    //   宏不配对会让撤销栈永久停在"宏内"状态，症状是"按一次撤销退好几步"。
    bool importImageAsLayer(const QString &path,
                            GrowPolicy policy = GrowPolicy::Ask);

    // 把当前文档的**合成结果**导出成 ICO 图标文件。
    //
    // ★ 为什么它是 public / 不带对话框 ★
    //   与 importImageAsLayer 完全同理：另存为的入口是 QFileDialog，
    //   而模态对话框在无人环境里会把进程永久挂住。把"选路径"留给
    //   调用方、把"真正干活"做成吃路径的函数，自检与截图工具才驱动得动。
    //
    // ★ 尺寸上限（512）是硬约束，超了一律拒绝 ★
    //   ICO 是图标格式，不是"另一种图片格式"。超过上限时返回 false 并
    //   在 errorOut 里给出**可以直接给用户看**的解释（含实际尺寸与上限，
    //   以及该怎么办）—— 文案由 engine/ico 统一提供，界面层不重复写数字。
    //
    // ★ 本函数不弹任何对话框 ★（弹窗是 writeImage 那条 UI 路径的事）
    bool exportIcon(const QString &path, QString *errorOut = nullptr);

    // ── 屏幕截图（「文件 → 屏幕截图」子菜单）──────────────────────────────
    //
    // ★ 为什么这里也是"拆成两半"的 ★
    //   抓屏幕这件事**依赖真实显示器**：自检跑在 offscreen 平台上，
    //   那里根本没有屏幕可抓。如果"建立新页"这一步只能跟在"真的抓一次"
    //   后面，这个功能就永远只能靠人眼验证 —— 与"带对话框的功能"是同一个
    //   困境（模态框会挂死 / 屏幕不存在），解法也一样：把**真正干活的那一半**
    //   （把一张内存里的图变成一页）做成公开入口，抓屏只是"图从哪来"之一。
    //   见 setScreenGrabber()。

    // 把一张**内存里的图**变成新的一页。不弹任何框、不写任何文件。
    // 返回新文档；失败（空图 / 建不出文档）返回 nullptr。
    //
    // ★ 与「文件 → 打开」共用同一份实现 ★
    //   "图 → 一页"在本工程里只有这一处。否则"图层 0 的名字从哪来"
    //   "路径必须在开页之前设好"（标签标题是从 filePath 现算的）这类规则
    //   会有两份，迟早只更新其中一份。
    struct ImagePageSpec {
        QImage  image;             // 必须非空
        QString filePath;          // 空 = 没有磁盘来源（屏幕截图就是这种）
        QString layerName;         // 图层 0 的名字；空 = 保留默认名
        QString titleHint;         // 无路径时的显示名（如"屏幕截图-1"）
        qreal   dpiX = 0.0;        // 0 = 不改（保持 Document 的默认值）
        qreal   dpiY = 0.0;
    };
    Document *createImagePage(const ImagePageSpec &spec);

    // ── 自动保存 / 崩溃恢复 ──
    //
    // 默认每 15 秒把"仍有未保存修改"的文档合成图写一份快照到用户数据目录，
    // 正常保存或正常退出时删掉。启动时若发现残留，问用户是否恢复。
    // 只存扁平图（多图层恢复后是单图层），见 core/RecoveryManager.h。

    void setAutoSaveIntervalMs(int ms);
    int  autoSaveIntervalMs() const;

    // 恢复目录。测试要指到临时目录，否则会污染真实用户数据。
    void setRecoveryDirectory(const QString &dir);

    // 启动后调用：有残留快照时询问是否恢复。
    // 不放在构造函数里，是为了让测试可以完全跳过这个模态框。
    void promptRecoveryOnStartup();

    // ── 撤销步数 ──
    //
    // 每个文档的撤销栈上限（1~200）。改完立刻对**所有已打开的页**生效，
    // 而不只是以后新开的页 —— 否则用户改完设置会发现"当前这张图没变"。
    // 超出的最旧记录被丢弃，这一点由 QUndoStack 自己保证。
    void setUndoLimit(int limit);
    int  undoLimit() const { return m_undoLimit; }

    // ── 打印与 PDF ──
    //
    // 与 importImageAsLayer / exportIcon 同一条纪律：带对话框的入口只负责
    // "选目标"，真正干活的这一半必须能脱离对话框被调用 —— 否则它永远
    // 只能靠人眼验证（模态框在离屏环境里会让进程永久挂住）。

    // 把当前文档的合成图铺到 printer 的第一页上（等比缩放、居中）。
    // printer 的输出格式、纸张、文件名都由调用方配好，这里不再改。
    // 返回 false = 没有文档 / 合成图为空 / QPainter 打不开设备。
    bool renderToPrinter(QPrinter *printer);

    // 导出单页 PDF。不弹任何对话框。
    // 纸张按图像的物理尺寸（像素 ÷ DPI）定，两边都带兜底夹紧。
    bool exportPdf(const QString &path, QString *errorOut = nullptr);

    // 立即抓取当前屏幕的内容，开为新的一页。
    //
    // 返回 false = 抓不到屏幕（没有显示器 / 平台拒绝）。此时**什么都不做**：
    // 不开空页、不动当前页，只把"为什么"说给用户听 ——
    // **交互式入口弹模态框**（Wayland 这种"这条路根本走不通"的情形，
    // 状态栏一闪而过的文字等于没说），**自动化入口只写状态栏**（没人点确定）。
    bool captureScreenNow();

    // 抓不到屏幕时到底要显示什么（标题 + 正文）。
    //
    // ★ 抽成函数是为了让"文案"与"怎么显示"分开 ★
    //   文案本身是要被自检断言的（Wayland 那句必须提到系统截图键与 Ctrl+V），
    //   但断言时**不能真的弹框** —— 离屏环境下一个模态框会把测试永久挂住。
    //   所以取值与显示分成两步，自检只需调这一个。
    static QString captureFailureTitle();
    static QString captureFailureText(CaptureBlock block);

    // ★ 弹框开关：默认 true，自检入口第一件事就是关掉它 ★
    //   不是"测试里记得别点那条路"，而是**结构上**保证离屏跑不到模态框。
    //   与 importImageAsLayer 的 GrowPolicy 同一条纪律。
    void setScreenCaptureDialogsEnabled(bool enabled);
    bool screenCaptureDialogsEnabled() const { return m_captureDialogs; }

    // 延迟抓取：**先把本窗口最小化**，到点抓屏，抓完自动恢复窗口。
    //
    // ★ 为什么必须自己先最小化 ★
    //   用户点"延迟 3 秒"的意思 100% 是"给我 3 秒切到目标窗口去"。
    //   不最小化的话，3 秒后抓到的就是本程序自己的窗口 —— 一张废图，
    //   而且用户只会认为"截图坏了"。
    //
    // delayMs 期间重复调用 = **重新计时**（不会攒出两张图）。
    void scheduleScreenCapture(int delayMs);

    // 还有多少毫秒要截（-1 = 没有待发的截图）。自检用。
    int pendingCaptureDelayMs() const { return m_captureDelayMs; }

    // 替换"抓屏幕"这一步（自检 / 截图工具用）。
    // 传空的 std::function = 恢复默认（真的去抓屏幕）。
    void setScreenGrabber(std::function<QImage()> grabber);

    // 全局快捷键对象（自检要验"登记成功没有""系统消息走不走得通"）。
    //
    // ★ 登记失败时它不是 nullptr ★
    //   对象一直都在，只是 isRegistered() 为假、lastError() 里有原因
    //   （例如被别的程序占用了）。"注册失败"绝不允许表现为"对象不存在"
    //   那种沉默 —— 否则上层会以为功能正常，而用户按了没反应。
    GlobalHotkey *globalHotkey() const { return m_hotkey; }

    // 立即截图的全局快捷键 —— "产品定的那一个"（候选表的第一项）。
    // 只能有一份定义：菜单、文档、登记、失败兜底都从它取。
    static constexpr const char *kCaptureHotkey = "Ctrl+Alt+S";

    // 全部候选，**首选在前**（第一项就是 kCaptureHotkey）。
    // setupScreenCapture() 逐个登记，第一个成功的即为实际生效键，
    // 并把这个键**写回菜单**（菜单上写的 = 真正能按的）。
    //
    // ★ 为什么允许回落，而不是死守一个键 ★
    //   全局热键是稀缺资源：同一个组合键全系统只能被一个进程登记，
    //   而 QQ / 微信 / 各家截图工具抢的恰好就是 Ctrl+Alt+S 这一类
    //   （实测开发机上这个键已被占用）。
    //   只留一个键的话，撞车的用户拿到的是"装好了、按下去永远没反应"——
    //   而这正是全局热键最典型的失败形态：与"程序坏了"无法区分。
    //   回落的代价只是状态栏多一句提示，换来的是在真实机器上**确实可用**。
    //
    // ★ 写成函数而不是静态数组 ★
    //   避免跨编译单元的静态初始化顺序问题；自检也能直接调它来断言
    //   "菜单上显示的键一定在候选表里"。
    static QStringList captureHotkeyCandidates();

    // 全局热键的登记 id（自检用它"伪造一次按键"，见 GlobalHotkey 的说明）
    static constexpr int kCaptureHotkeyId = 1;

    // ── 批量打开：命令行参数与"拖入多个文件"共用 ──
    //
    // 逐个开成一页。**已经打开过的文件不重复开**，只切到已有那一页 ——
    // 同样的文件开出两份独立副本，会让"在两个标签里各改一遍、保存时
    // 后者覆盖前者"变成真的。
    //
    // 返回成功打开的页数（含"切到已有页"的次数）。
    //
    // ★ 为什么命令行与拖放必须共用它 ★
    //   两者的语义完全相同（"给我一串路径，每个开一页"）。
    //   各写一份的话，"重复文件怎么办"这类规则必然只有一份会更新到位，
    //   而两份代码的差异只有真的拖多个文件进来才看得到。
    int openDocumentPaths(const QStringList &paths);

protected:
    // 关闭窗口时检查未保存的修改
    void closeEvent(QCloseEvent *event) override;

    // 拖文件到窗口打开
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    // 快捷键（工具的单键快捷键需要在这里拦截 —— 见 keyPressEvent 说明）
    void keyPressEvent(QKeyEvent *event) override;

    // macOS：系统把"用本程序打开文件"投成 QEvent::FileOpen 事件，
    // 而不是命令行参数（拖到 Dock 图标、Finder 里选"打开方式"都走这条）。
    // 事件发给 QApplication，所以过滤器装在 qApp 上，不在窗口自己身上。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ── 初始化 ──

    void setupDocumentManager();
    void setupTools();
    void setupCanvas();
    // 每新建一块画布都要调一次（画布是"一页一块"的）—— 见 .cpp 说明。
    void connectCanvas(CanvasView *canvas);
    void setupActions();
    void setupMenus();
    // 语言切换的"重启生效"提示框（保存动作在 UiLanguage::setCurrentLanguage
    // 里已落盘，这里只负责把话说清楚）。
    // ★ 为什么是成员函数而不是 setupMenus 里的局部 lambda ★
    //   connect 存起来的是 lambda 的闭包：局部 lambda 引用的也是局部
    //   变量，setupMenus 一返回那些空间就失效了 —— 点击菜单时调的会是
    //   一块已销毁的栈内存。而"点了没反应/直接崩"要等到真去点才现形，
    //   双构建 0 错 0 警。成员函数的寿命跟对象一样长，没有这个问题。
    void promptRestartForLanguage(const QString &displayNameText);
    void setupToolBars();
    void setupDocks();
    void setupStatusBar();

    // 自动保存：建立管理器与定时器（在 setupDocumentManager 里调用）。
    void setupRecovery();
    void scheduleRecovery();
    void writeRecoverySnapshots();
    void discardRecoveryFor(Document *doc);
    QString recoveryKeyFor(Document *doc);

    // ★ 屏幕截图的两件"活物"：全局热键与延迟定时器 ★
    //   必须在 setupStatusBar() **之后**调用 —— 热键登记失败时要用状态栏
    //   把原因说出来（登记失败最常见的形态就是"被别的程序占用了"，
    //   而那正是"按了没反应"最容易被误认为程序坏了的地方）。
    //   内部顺序也是定死的：先 connect、后 register（见 .cpp）。
    void setupScreenCapture();

    // 把"实际生效的那个键"写到「立即截图」动作上（快捷键 + 提示文字）。
    //
    // ★ 为什么必须有一个函数来做这件事，而不是在两处各写一遍 ★
    //   setupActions() 建动作时先按首选键显示（那时还没登记，只能这么办），
    //   setupScreenCapture() 登记完可能发现首选键被占、换成了别的键。
    //   于是这个组合键有**两个时刻**的值：桌面上的说明文字必须跟着
    //   后者走。两处各写一遍的话，迟早出现"菜单写 A、按 B 才管用"。
    void applyCaptureHotkey(const QKeySequence &sequence);

    // 延迟截图的定时器到点了（抓屏 → 恢复窗口）。私有：只给定时器用。
    void onCaptureTimerFired();

    void setupConnections();

    // 加载/保存窗口状态（几何、停靠位置、上次的工具）
    void loadSettings();
    void saveSettings();

    // ── 文档切换（多标签）──
    //
    //  ★ 阶段八的核心不变式 ★
    //
    //     m_doc / m_commandStack / m_canvas 这三个指针**永远指向"当前页"**
    //     的那三件套 —— 它们没有变成"每页一套的数组"，而是原地保留了
    //     "当前"的语义。
    //
    //     这是本阶段改动量小的根本原因：全工程几百处
    //     `m_doc->xxx()` / `m_canvas->xxx()` / `m_commandStack->xxx()`
    //     一行都不用改，因为它们问的本来就是"当前那一页"。
    //     切页时只做一件事：把这三个指针重新指向新页，然后走一遍
    //     applyDocumentToPanels()。
    //
    //     如果改成"每处调用都写 m_tabs->currentTab()->doc"，那这几百处
    //     全都要改 —— 而且每一处都是一个"忘了改就编译不过"的机会。

    // 为 doc 开一页（建好它自己的撤销栈与画布），并切到它。
    // 返回新页下标（< 0 表示失败）。
    int attachDocumentTab(Document *doc);

    // ★ "当前页"唯一的变化点 ★
    //
    //  重指向三件套 → 把活动工具与颜色推给新画布 → applyDocumentToPanels()
    //  → 同步撤销动作 → 更新标题。
    //  传 -1 表示"没有任何页"（全部关闭后的空状态）。
    //
    //  ★ 为什么必须是唯一入口 ★
    //    少走一步的症状都很隐蔽：忘了推活动工具 → 新页上鼠标毫无反应
    //    （与"工具没注册"症状一模一样）；忘了同步撤销动作 → 撤销菜单项
    //    的可用状态停在上一个页。这类"少做一件事"的 bug 只能靠
    //    "变化点只有一个"来根治。
    void activateTab(int index);

    // 关闭第 index 页（含该页文档的未保存确认）。
    // 用户取消时**什么都不做** —— 页面原样留着，与 closeEvent 的
    // e->ignore() 是同一个语义。
    void closeDocumentTab(int index);

    // ★ 遵守约定 7 的核心函数 ★
    //   把新文档推给所有需要它的部件。集中在这里，
    //   新增面板时只要在这里加一行，就不会遗漏。
    void applyDocumentToPanels(Document *doc);

    // ★ 撤销/重做动作与"当前栈"的绑定 ★
    //
    //  连接在**每个栈创建时**一次性建好，回调里用
    //  `stack != m_commandStack` 过滤掉非当前页的栈。
    //
    //  ★ 为什么不"换页时断开旧栈、连上新栈" ★
    //    那样必须把"上一个连过的栈"记在一个成员/动态属性里，然后在
    //    切换时 disconnect。而 disconnect 之后、连接新栈之前的任何一次
    //    取用都可能踩到已经析构的旧栈（关掉一页的瞬间正是这个时刻）。
    //    改成"连一次、按栈过滤"之后，连接的寿命就等于栈的寿命，
    //    不存在"忘记断开"或"断到野指针"这两种失败模式。
    void wireCommandStack(CommandStack *stack);

    // 把栈的当前状态**显式**推给撤销/重做动作（可用状态与文本）。
    //
    //  ★ 为什么信号不够用 ★
    //    canUndoChanged 只在**变化**时发。从"能撤销的 A 页"切到
    //    "不能撤销的 B 页"，B 的栈从来没发过信号，菜单项会停在上一个
    //    页的状态上 —— 明明不能撤销，菜单却是亮的。
    void syncUndoRedoActions(CommandStack *stack);

    // ★ 撤销栈销毁**之前**的统一解绑点 ★
    //
    //  有些部件持有 CommandStack* 裸指针，而且会在任意时刻被回调：
    //    · 历史面板 QUndoView —— 内部模型存的是**裸** QUndoStack*，
    //      换绑时会先对旧栈 disconnect()；
    //    · 状态栏 —— Document 一发 contentChanged 就会回调到
    //      refreshDocumentInfo()，那里要读栈的内存占用。
    //
    //  栈被销毁后它们若不先解绑，就是明确的 use-after-free
    //  （实测症状：关掉一个标签页 → 段错误）。
    //  本函数由 DocumentTabs::aboutToRemoveTab 驱动，时机是
    //  "栈还活着、但马上要没"。
    //
    //  ★ 只解开"确实指向这一份栈"的消费者 ★
    //    不加这层判断的话，销毁后台页的栈会把当前页的绑定也一起打断 ——
    //    而 closeEvent / clearAllTabs 那条路径上**没有后续的重新绑定**。
    //    （唯一例外是析构：那时全部都要解开，所以 dyingStack == nullptr
    //      被定义为"全部解开"。）
    void detachStackConsumers(CommandStack *dyingStack);

    // 更新窗口标题（文件名 + 修改标记 + 缩放比）
    void updateWindowTitle();

    // ── 文件操作 ──

    void newDocument();
    void openDocument();
    // 把 path 指向的图片开成**一页**（新文档 + 新标签）。
    // 返回 true 表示"现在用户看的确实是这张图"（新开了一页，或切到了
    // 已经打开的那一页）；false 表示文件不可读/用户拒绝了大图提示。
    bool openDocumentFromPath(const QString &path);

    // 「文件 → 打开为图层」：把另一张图片整幅作为新图层加进当前文档。
    //
    // ★ 为什么需要它（以及它为什么能替代多文档）★
    //   "从图 B 框选一块贴到图 A"这个需求，缺的从来不是"复制/粘贴"
    //   （复制走系统剪贴板、粘贴为新图层早已实现），而是**同时看到两张图**。
    //   本功能把 B 整幅变成一个图层，于是"跨文档"问题退化成"跨图层"问题 ——
    //   而选区、删除、移动这些图层内操作全都是现成的。
    //   代价是要多做一步（在该层上框选后反选 + 删除），换来的是零架构改动。
    //   多标签页是同一需求的正解，但它要重构中央控件，所以排在后面单独做。
    //
    // ★ 本函数现在只剩"选文件"这一步 ★
    //   真正的导入逻辑搬去了 importImageAsLayer()（public，见上），
    //   因为 QFileDialog 会让自动化环境卡死 —— 见那边的说明。
    void openAsLayer();

    bool saveDocument();
    bool saveDocumentAs();
    void closeCurrentDocument();

    // 「文件 → 打印…」：选打印机与纸张，再把当前页交给 renderToPrinter()。
    void printDocument();

    // 「文件 → 导出 PDF…」：选路径，再交给 exportPdf()。
    void saveAsPdf();

    // 用 QImageWriter 写文件（而非 QImage::save），以便设置格式参数
    bool writeImage(const QString &path, const QString &format,
                    int quality, bool includeAlpha);

    // 如果有未保存的修改，询问用户。返回 false 表示用户取消了操作。
    // 作用对象是**当前页**（m_doc）。
    bool maybeSave();

    // 逐页问过去，任何一页被取消就整体返回 false。
    //
    // ★ 为什么需要一个"全部"版本 ★
    //   关窗口时只问当前页是不够的：另外三页里可能有一页改了没保存，
    //   用户点"关闭"之后它就随着进程一起没了，而且没有任何提示。
    //   实现上它会把"有未保存修改的那一页"先切到前面再调用 maybeSave() ——
    //   复用"当前页"那一套（保存、另存为、取消）而不是再写一份
    //   吃 Document* 的重载：那等于把几百行整体复制一遍，且没有自检守着。
    bool maybeSaveAll();

    // ── 编辑操作 ──

    void undo();
    void redo();

    // 「编辑 → 撤销步数…」：问一个数字，交给 setUndoLimit() 落地。
    // 对话框与实现分开，理由同 importImageAsLayer：模态框在自动化环境会卡死。
    void openUndoLimitDialog();

    void cutSelection();
    void copySelection();
    void pasteAsNewLayer();
    void pasteIntoLayer();
    void clearSelectionContent();

    void selectAll();
    void deselect();
    void invertSelection();

    void onDeleteSelection();

    // ── 图像调整与滤镜 ──
    // 不需要参数的两项（去色/反相）直接执行；
    // 需要参数对话框的走 runAdjustment() 的统一流程。
    // 曲线必须有（用户明确表示高频使用），见 docs/曲线调整设计.md。

    void applyGrayscale();
    void applyInvert();

    void openBrightnessContrast();
    void openHueSaturation();
    void openCurves();
    void openLevels();

    // ── 调整对话框的统一流程 ──
    //
    // ★ 预览机制 ★
    //   拖动参数时，运算发生在**降采样副本**（最长边 ≤ kAdjustPreviewMaxSide）上，
    //   结果交给 CanvasView 作为"临时呈现图"显示 —— 文档从头到尾没有被修改过。
    //   这带来两个好处：
    //     · 每帧只换算约 100 万像素，6~10ms，滑块跟手
    //     · "取消"不需要任何还原逻辑，天然不会留下脏数据
    //   点确定时才在**全尺寸原图**上重算一次并压入一条 ImageCommand。
    //
    //   base —— 调用方已经取好的"操作前图层快照"（也是对话框构造时用的
    //           source，因此不必再拷一份：4000×3000 的一次拷贝就是 48MB）。
    bool runAdjustment(AdjustmentDialog *dialog, const QImage &base,
                       const QString &historyText);

    // 捕获调整操作的源图。失败（无文档 / 图层锁定）时返回空图并给出提示。
    QImage prepareAdjustmentSource();

    // 对话框发来 previewNeeded 时调用：从预览基准图重算并交给画布
    void updateAdjustmentPreview();

    // 旋转对话框专用：重算降采样旋转结果并交给画布（含预览画布尺寸）。
    //
    // ★ 为什么不复用 updateAdjustmentPreview ★
    //   调整类预览只是"换一张同样尺寸的图"，而旋转会**改变画布尺寸**，
    //   要连预览画布尺寸一起交给画布（见 CanvasView::setPreviewImage 重载）。
    //   更关键的是两者的收尾完全不同：调整压的是"单层像素替换"命令，
    //   旋转压的是"全图层 + 画布尺寸"的 GeometryChangeCommand。
    //   把两条路塞进一个函数只会让分支越来越多。
    void updateRotatePreview();

    // ── 尺寸 / 旋转 ──

    // 图像大小 / 画布大小。两个对话框都**不预览**（理由见
    // ImageSizeDialog.h 开头的说明），确定时一次性完成并压入一条撤销。
    void openImageSize();
    void openCanvasSize();

    void rotate90CW();
    void rotate90CCW();
    void rotate180();

    // 任意角度旋转。与上面三个不同，它**有对话框与实时预览**：
    // 任意角度必然重采样且不可逆，用户需要先看到结果再确认。
    void openRotateArbitrary();

    // ── 裁剪 ──
    //
    // 两个入口，一段收尾：
    //   · 工具箱的裁剪工具（C）在画布上拖出矩形 → 发 cropRequested →
    //     applyCropFromTool()
    //   · 菜单「图像 → 裁剪」把画布收成**当前选区的包围盒** → cropToSelection()
    //
    // ★ 为什么两个入口都落在 MainWindow，而不是让工具自己压命令 ★
    //   几何类操作（缩放 / 旋转 / 翻转 / 裁剪）的收尾完全相同：快照 →
    //   执行 → 内存预检 → 压栈 → 视图适配 → 状态栏。其中最不能复制的
    //   是**内存预检**那道防线 —— 它只能有一个入口（见下面
    //   commitGeometryChange() 的注释）。工具自己 new 一个
    //   GeometryChangeCommand 就等于开了第二个入口，而漏掉预检的代价
    //   要到内存耗尽时才暴露。
    void applyCropFromTool(const QRect &rect);
    void cropToSelection();

    void flipHorizontal();
    void flipVertical();

    // ── 图层操作 ──

    void addNewLayer();
    void duplicateCurrentLayer();
    void deleteCurrentLayer();
    void moveLayerUp();
    void moveLayerDown();
    void mergeLayerDown();
    void flattenImage();
    void mergeVisibleLayers();

    // ── 图层蒙版 ──
    //
    // 用当前选区生成蒙版：选中处露出、未选中处遮住。蒙版不改图层像素，
    // 所以随时可以删掉还原 —— 这是它相对"直接删像素"的全部价值。
    void addLayerMaskFromSelection();
    void removeLayerMask();

    /// 让图层面板里活动图层那一行进入重命名编辑态
    void renameActiveLayer();

    // ── 下面这些由 LayerPanel 的信号驱动 ──
    //
    //  ★ 为什么"当前图层"版本（duplicateCurrentLayer 等）和"指定索引"版本
    //    要并存 ★
    //    前者来自菜单/快捷键，作用对象是"活动图层"，语义固定；
    //    后者来自面板，作用对象是**面板上被选中的若干行**——在
    //    ExtendedSelection 下这可能不是活动图层（可以框选多行、
    //    甚至多选里不含活动图层）。
    //    如果让面板先改活动图层再调前者，就会**污染用户的选择状态**：
    //    点一下"删除"顺手把活动图层换掉了。所以两条路径分开。
   
    /// 删除选中的若干图层（面板多选）。列表为空时退回"删除活动图层"。
    void deleteLayersFromPanel(const QList<int> &layerIndices);
    /// 复制选中的若干图层（面板多选）。列表为空时退回"复制活动图层"。
    void duplicateLayersFromPanel(const QList<int> &layerIndices);
    /// 把**指定**图层向下合并（面板某一行）
    void mergeLayerDownAt(int layerIndex);
    /// 拖拽排序：from/to 都是文档索引
    void reorderLayer(int fromIndex, int toIndex);

    // ── 图层属性（面板的眼睛 / 锁 / 名字 / 混合模式 / 不透明度）──
    void setLayerVisible(int layerIndex, bool visible);
    void setLayerLocked(int layerIndex, bool locked);
    void setLayerName(int layerIndex, const QString &name);
    void setLayerBlendMode(int layerIndex, int blendBefore, int blendAfter);
    /// 不透明度拖动中：只改内存、不压栈（等同于其它对话框的实时预览）
    void previewLayerOpacity(int layerIndex, int opacity);
    /// 不透明度松手：压一条命令
    void commitLayerOpacity(int layerIndex, int before, int after);

    // ── 视图操作 ──

    // 全屏切换（F11）。退出时恢复到进入前的状态（最大化 / 普通），
    // 而不是一律还原成普通窗口 —— 全屏前最大化的用户会突然丢掉大窗口。
    void toggleFullScreen();

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToActualSize();
    void setZoomLevel(qreal level);

    void toggleGrid(bool visible);
    void toggleCheckerboard(bool visible);

    // ── 网格的间距与吸附（阶段七收尾）──

    // 「对齐到网格」开关。应用级偏好，存 QSettings。
    // ★ 与「显示网格」相互独立 ★ —— 见 CanvasView::setSnapToGrid 的说明。
    void toggleSnapToGrid(bool enabled);

    // 选一个预设间距（来自「网格间距」子菜单的 6 个单选项）。
    void setGridSpacing(int pixels);

    // 「自定义…」：开 GridSettingsDialog，取回间距与线色再推给当前画布。
    void editGridSettings();

    // ── 视图辅助：标尺与参考线（阶段七）──

    // 显示/隐藏标尺（上下/左右/拐角一起）。应用级偏好，存 QSettings。
    void toggleRuler(bool visible);

    // 切换标尺单位。index 对应 rulermetrics::Unit（与菜单顺序一致）。
    void setRulerUnit(int index);

    // 显示 / 锁定 / 清除参考线（都是应用级偏好 —— 除了"清除"，
    // 它作用在**当前页**的参考线上）。
    void toggleGuides(bool visible);
    void toggleGuidesLocked(bool locked);
    void clearGuides();

    // 按快捷键激活工具
    bool activateToolByShortcut(int key);

    // ── 工具结果处理 ──

    // 工具请求更新前景色/背景色
    void setForegroundColor(const QColor &c);
    void setBackgroundColor(const QColor &c);
    void chooseForegroundColor();
    void chooseBackgroundColor();
    void swapColors();
    void resetColors();

    // ── 无文档时的统一保护 ──

    // 是否有一个可用的文档。没有时返回 false 并给出状态栏提示。
    // ★ 每个文档相关的槽函数第一行都应该调它 ★
    //   否则用户在没有打开文档时点菜单会崩溃 ——
    //   这是"菜单全部禁用"之外的第二种保护（双保险）。
    bool requireDocument();

    // ── 成员 ──

    DocumentManager *m_docManager = nullptr;
    CommandStack    *m_commandStack = nullptr;
    Document        *m_doc = nullptr;         // 当前页的文档

    // ★ 中央控件：多标签容器（不是单个 CanvasView）★
    //   它是"页 ⇄ (文档, 撤销栈, 画布)"的唯一账本。
    DocumentTabs    *m_tabs = nullptr;

    // ★ 正在"摘页 / 增页"的中间态，期间屏蔽标签容器的信号 ★
    //
    //   QTabWidget 在 addTab/removeTab 时会**自己**发 currentChanged，
    //   而那一刻"标签序号"与"文档序号"可能对不上（一个动过了、另一个
    //   还没动）。不屏蔽的话，同步逻辑会把当前文档指到错误的那一个上 ——
    //   而且不报任何错，症状是"关掉一页之后，标题显示的是别的文件"。
    //   结构变更结束后再显式 activateTab() 补推一次当前状态。
    bool             m_switchingTabs = false;

    CanvasView      *m_canvas = nullptr;      // 当前页的画布
    ToolManager     *m_toolManager = nullptr;
    ToolOptionsBar  *m_optionsBar = nullptr;
    StatusBarWidget *m_statusBar = nullptr;

    // ── 视图辅助：标尺（阶段七）──
    //
    // ★ 为什么标尺是"两把 + 一个拐角"，而不是画布内部的东西 ★
    //   标尺必须贴在画布**外面**（上边一条、左边一条），而画布是每页一块的
    //   （装在 QTabWidget 的页面里）。所以宿主是 MainWindow 的中央控件：
    //   一个 QGridLayout —— 拐角 / 上标尺 / 左标尺 / 标签容器。
    //   标尺本身**不知道**自己属于哪一页，它只跟着 activateTab() 被 setView() 重指。
    //
    // ★ 为什么"显示标尺"要连拐角一起管 ★
    //   拐角是左上角那个小方块，功能上只是"补上两把尺子交会处的空缺"。
    //   尺子藏了它还留着，就是一个孤零零的灰方块 —— 看起来像渲染错误。
    QWidget     *m_canvasHost  = nullptr;   // 中央控件的网格宿主
    QWidget     *m_rulerCorner = nullptr;   // 左上角小方块
    RulerWidget *m_hruler      = nullptr;   // 上边（水平，管 x）
    RulerWidget *m_vruler      = nullptr;   // 左边（垂直，管 y）

    QDockWidget     *m_toolBoxDock = nullptr;
    QDockWidget     *m_layerDock = nullptr;
    QDockWidget     *m_historyDock = nullptr;
    QDockWidget     *m_colorDock = nullptr;
    QDockWidget     *m_navigatorDock = nullptr;

    LayerPanel      *m_layerPanel = nullptr;
    NavigatorPanel  *m_navigatorPanel = nullptr;

    QUndoView       *m_undoView = nullptr;

    // 自动保存 / 崩溃恢复。定时器是单次型的：存完若还有脏文档就再续一次。
    RecoveryManager *m_recoveryManager = nullptr;
    QTimer          *m_recoveryTimer = nullptr;
    int              m_recoverySeq = 0;
    QHash<Document *, QString> m_recoveryKeys;

    QToolBar        *m_mainToolBar = nullptr;
    QToolBar        *m_toolsToolBar = nullptr;

    // ── 全部 QAction ──
    // ★ 用成员变量而不是局部变量 ★
    //   菜单项、工具栏按钮、右键菜单可能引用同一个 QAction，
    //   而且需要根据文档状态启用/禁用它们。局部变量做不到这些。

    QAction *m_actionNew = nullptr;
    QAction *m_actionOpen = nullptr;
    QAction *m_actionOpenAsLayer = nullptr;   // 文件 → 打开为图层

    // 文件 → 屏幕截图（子菜单里的四项）
    //   ★ 为什么四个都是独立动作，而不是"一个动作 + 参数" ★
    //     菜单项的触发是"点击"这一个动作，参数只能靠 lambda 捕获延时；
    //     而自动化与对账脚本要按 objectName 找到**每一项**（见 action_check.py
    //     与 mainwin_test 的"每个有名字的菜单动作都有图标"）。四个动作各自
    //     有名字、有图标，代价只是三行声明。
    QAction *m_actionCaptureNow = nullptr;       // 立即截图（带全局快捷键）
    QAction *m_actionCaptureDelay3 = nullptr;
    QAction *m_actionCaptureDelay5 = nullptr;
    QAction *m_actionCaptureDelay10 = nullptr;

    QAction *m_actionSave = nullptr;
    QAction *m_actionSaveAs = nullptr;

    // 文件 → 打印… / 导出 PDF…
    QAction *m_actionPrint = nullptr;
    QAction *m_actionExportPdf = nullptr;
    QAction *m_actionClose = nullptr;
    QAction *m_actionQuit = nullptr;

    QAction *m_actionUndo = nullptr;
    QAction *m_actionRedo = nullptr;
    QAction *m_actionCut = nullptr;
    QAction *m_actionCopy = nullptr;
    QAction *m_actionPaste = nullptr;
    QAction *m_actionPasteInto = nullptr;
    QAction *m_actionDelete = nullptr;

    // 编辑 → 撤销步数…
    QAction *m_actionUndoLimit = nullptr;

    QAction *m_actionSelectAll = nullptr;
    QAction *m_actionDeselect = nullptr;
    QAction *m_actionInvertSelection = nullptr;

    QAction *m_actionGrayscale = nullptr;
    QAction *m_actionInvert = nullptr;

    QAction *m_actionBrightnessContrast = nullptr;
    QAction *m_actionHueSaturation = nullptr;
    QAction *m_actionCurves = nullptr;
    QAction *m_actionLevels = nullptr;

    QAction *m_actionImageSize = nullptr;
    QAction *m_actionCanvasSize = nullptr;

    QAction *m_actionRotate90CW = nullptr;
    QAction *m_actionRotate90CCW = nullptr;
    QAction *m_actionRotate180 = nullptr;
    QAction *m_actionRotateArbitrary = nullptr;
    QAction *m_actionFlipH = nullptr;
    QAction *m_actionFlipV = nullptr;
    QAction *m_actionCrop = nullptr;          // 图像 → 裁剪（裁到选区包围盒）

    QAction *m_actionLayerNew = nullptr;
    QAction *m_actionLayerDuplicate = nullptr;
    QAction *m_actionLayerDelete = nullptr;
    QAction *m_actionLayerMoveUp = nullptr;
    QAction *m_actionLayerMoveDown = nullptr;
    QAction *m_actionMergeDown = nullptr;
    QAction *m_actionFlatten = nullptr;
    QAction *m_actionLayerMergeVisible = nullptr;
    QAction *m_actionLayerRename = nullptr;

    // 图层 → 蒙版
    QAction *m_actionLayerMaskAdd = nullptr;
    QAction *m_actionLayerMaskRemove = nullptr;

    QAction *m_actionZoomIn = nullptr;
    QAction *m_actionZoomOut = nullptr;
    QAction *m_actionZoomFit = nullptr;
    QAction *m_actionZoomActual = nullptr;
    QAction *m_actionShowGrid = nullptr;
    QAction *m_actionShowCheckerboard = nullptr;

    // 视图 → 全屏（F11）
    QAction *m_actionFullScreen = nullptr;

    // ── 网格的间距与吸附（阶段七收尾）──
    QAction *m_actionSnapToGrid = nullptr;

    // 「网格间距」子菜单的 6 个预设项（顺序与 kGridPresets 一致）。
    // 与「标尺单位」同套路：靠**下标**映射到毫米值，不靠逐个 isChecked()。
    QList<QAction *> m_actionGridSpacing;
    QAction *m_actionGridCustom = nullptr;

    // 网格间距 / 线色 / 吸附的**单一来源**。
    //
    // ★ 为什么放在 MainWindow、而不是只存在画布里 ★
    //   画布是**每页一份**的：新开一页/切页时新画布拿不到旧画布的值。
    //   而这三个是"应用级偏好"（存 QSettings、跨页一致），所以必须有一个
    //   比画布活得久的地方存着，切页时由 applyDocumentToPanels() 往下推
    //   （与 m_foregroundColor 完全同一条规律）。
    int    m_gridSpacing = 64;
    QColor m_gridColor   = QColor(120, 120, 120, 110);

    // ── 视图辅助：标尺与参考线 ──
    QAction *m_actionShowRuler   = nullptr;
    QAction *m_actionShowGuides  = nullptr;
    QAction *m_actionLockGuides  = nullptr;
    QAction *m_actionClearGuides = nullptr;

    // 「标尺单位」子菜单的 5 个互斥项，**顺序必须与 rulermetrics::Unit 一致**
    // （像素 / 厘米 / 毫米 / 英寸 / 点）。取 checkedAction 的下标就能直接当
    // 枚举用 —— 用下标而不是"逐个 isChecked() 问一遍"，是因为后者一旦漏问
    // 某一项，那个单位就永远选不中（而且不报错，只是"点了没反应"）。
    QList<QAction *> m_actionRulerUnit;

    QAction *m_actionSwapColors = nullptr;
    QAction *m_actionResetColors = nullptr;
    QAction *m_actionForegroundColor = nullptr;
    QAction *m_actionBackgroundColor = nullptr;

    QAction *m_actionAbout = nullptr;
    QAction *m_actionAboutQt = nullptr;

    // 面板显隐动作在 setupDocks 里通过 dock->toggleViewAction() 生成，
    // 不需要额外成员 —— 菜单项直接引用它们。

    // 工具 QAction 的映射（用于工具箱面板与快捷键）
    QHash<QString, QAction *> m_toolActions;

    // 文档内嵌的"最近使用文件"上限
    static constexpr int kMaxRecentFiles = 10;
    QList<QAction *> m_recentFileActions;

    // ── 编辑偏好 ──

    // 撤销步数上限。开新页时推给它的撤销栈，改设置时推给全部已有的栈。
    int m_undoLimit = 30;

    // 进入全屏前是否处于最大化。退出全屏时据此还原 ——
    // 一律 showNormal() 会让"最大化后按 F11 再按 F11"丢掉大窗口。
    bool m_wasMaximizedBeforeFullScreen = false;

    // ── 屏幕截图的状态（装配在 setupScreenCapture 里）──

    // 全局热键。★ 它不需要"有没有注册成功"来当判据：看 isRegistered() ★
    GlobalHotkey *m_hotkey = nullptr;

    // 延迟截图的单次定时器（只有一个 —— 重复点延迟项 = 重新计时，
    // 而不是攒出好几个定时器然后一次截好几张）
    QTimer *m_captureTimer = nullptr;

    // 还有多少毫秒要截；-1 = 没有待发的截图（也是"上一张已经抓完"的标志）
    int m_captureDelayMs = -1;

    // 这次延迟截图是我们自己把窗口最小化的吗？
    // 是 → 抓完要恢复窗口；不是（用户本来就把它最小化了）→ 抓完别乱动用户的窗口。
    bool m_captureMinimizedSelf = false;

    // 屏幕截图页的序号（"屏幕截图-1"、"屏幕截图-2"…）。
    // ★ 只增不减 ★：关掉第一张之后下一张是 -2 而不是又回到 -1 ——
    //   编号复用会让"我记得刚才那张叫 -1"变成一句没法验证的话。
    int m_screenshotSeq = 0;

    // "抓屏幕"这一步的实现。默认空 = 真的去抓（compat::grabScreen）。
    // 自检用 setScreenGrabber() 换成一张造好的图，于是整条
    // "定时器/热键 → 抓 → 建页" 都能在 offscreen 上跑通。
    std::function<QImage()> m_screenGrabber;

    // 抓不到屏幕时允不允许弹模态框（默认允许）。
    // 自检会把自检入口设成 false —— 见 setScreenCaptureDialogsEnabled()。
    bool m_captureDialogs = true;

    // 失败弹框是否已经排在事件队列里。
    // 连点两次"立即截图"时不该叠出两个一模一样的失败框。
    bool m_captureFailureDialogPending = false;

    // 延迟截图：先最小化再等 —— 公开入口是 scheduleScreenCapture()，
    // 四个菜单项直接调它（不再包一层私有同名函数，少一个名字少一处不一致）。
    // 定时器到点后走 onCaptureTimerFired()。

    // ── 最近文件列表管理 ──
    QStringList recentFiles() const;
    void        addRecentFile(const QString &path);
    void        updateRecentFileActions();

    // ── 工具箱面板的构建（在 setupTools 里被调用）──
    //
    // ★ 调用点不能挪回 setupToolBars ★
    //   它遍历 ToolManager 的工具列表建按钮，必须等工具注册完；
    //   写在 setupToolBars 里会因 m_toolManager 为空而静默 return，
    //   工具箱将是一片空白（实测踩过，见 .cpp 里的注释）。
    void buildToolBox();

    // ── 活动工具 → UI 的唯一同步入口 ──
    //
    //  画布、选项条、工具箱按钮的选中态三处都跟着"当前工具"走。
    //  两个调用点：ToolManager::activeToolChanged 信号，
    //  以及 setupTools() 收尾处的一次显式推送
    //  （信号传不了"连接建立之前已经发生"的状态，见 .cpp 注释）。
    void applyActiveToolToUi(AbstractTool *tool);

    // ── 颜色指示器同步 ──
    // 颜色显示在多处（工具箱底部、颜色面板的两个色块），
    // 集中在一个函数里更新，避免"改了一处漏了另一处"。
    void updateColorIndicators();

    // ── 剪贴板 ──

    // 更新"粘贴"动作的可用状态（根据系统剪贴板内容）
    void updatePasteActionState();

    // 生成一个字母占位图标（工具还没提供 SVG 图标时的后备方案）
    QIcon makeLetterIcon(const QString &text) const;

    // ── 内部辅助 ──

    // 取活动图层的位图副本（整层快照式命令的 "before" 状态）
    QImage captureActiveLayerImage() const;

    // 压入一个"整层快照"命令（滤镜、去色、反相共用）
    void pushActiveLayerImageCommand(const QImage &before,
                                     const QImage &after,
                                     const QString &text);

    // 几何类操作（旋转/翻转）的统一实现。
    //
    //  ★ 为什么统一 ★
    //    这五种操作的撤销逻辑完全相同：存"所有图层 + 画布尺寸 + 选区"
    //    的前后快照。分开写五遍会产生五份几乎一样的、都很容易写错的代码。
    enum class GeoOp { Rotate90CW, Rotate90CCW, Rotate180, FlipH, FlipV };

    // 执行几何操作并记录撤销。返回是否成功。
    bool performGeometryOp(GeoOp op);

    // ★ 几何/尺寸类操作共同的收尾流程 ★
    //
    //   取 after 快照 → 内存预检 → 压入撤销栈 → 视图适配 → 状态栏提示。
    //
    //   抽出来的理由只有一个：**内存预检这道防线不能有第二个入口**。
    //   尺寸类对话框（图像大小 / 画布大小）与这里的旋转/翻转走的是完全
    //   相同的收尾；若各写一遍，将来新增第三个几何入口时几乎必然会漏掉
    //   预检 —— 而漏掉的代价是"用户点一次确定就被几百 MB 的快照占住"，
    //   并且要到内存耗尽时才会暴露。
    //
    //   调用前提：before 由 GeometryChangeCommand::captureState() 取得，
    //             且文档上的操作**已经执行完毕**。
    bool commitGeometryChange(const GeometryChangeCommand::Snapshot &before,
                              GeometryChangeCommand::Kind kind,
                              const QString &text);

    // 从当前文档与选区构造剪贴板数据。
    // ★ 关键点：把选区蒙版值乘进 alpha 通道 ★
    //   否则选区外的内容会一起被复制 —— 这是最容易漏掉的一步。
    ClipboardData buildClipboardData() const;

    // 把剪贴板内容贴进来。asNewLayer 为 true 时新建图层。
    bool pasteFromClipboard(bool asNewLayer);

    // ★ "把一张图变成一个新图层"的唯一实现 ★
    //
    //   image  —— 要放进去的像素
    //   pos    —— 放在画布上的哪个位置（画布坐标）
    //   name   —— 新图层名（粘贴用"粘贴的图层"，导入用文件名）
    //
    //   契约：
    //     · 新图层的位图**填满画布**（本工程图层统一尺寸模型），图像画在 pos 处
    //     · 只压**一条** LayerCommand，不自己 beginMacro —— 撤销粒度由调用方定
    //       （粘贴是一条，导入是"扩画布 + 加图层"两条合成一条宏）
    //     · 成功后把新图层设为活动图层
    //
    //  ★ 为什么必须是"唯一实现" ★
    //    粘贴与导入要做的是**同一件事**（一张图 → 一个新图层），
    //    只是像素来源与名字不同。各写一遍的话，"图层要填满画布"
    //    "用 CompositionMode_Source 而不是 SourceOver"这类约束
    //    会被复制成两份，改一处忘一处 —— 而且第二份没有自检守着。
    bool addImageAsNewLayer(const QImage &image, const QPoint &pos,
                            const QString &layerName, const QString &historyText);

    // 按选区蒙版混合"处理前"与"处理后"的位图。
    //
    //  ★ 这是"只处理选区内像素"的通用实现 ★
    //    所有整层滤镜/调整都复用它（去色、反相、以及阶段四新增的各种调整）。
    //
    //    不在 engine 层实现的原因：那会让 engine 依赖 core 的 Selection，
    //    破坏"engine 只依赖 QtGui"的单向依赖铁律。
    //
    //  公式：结果 = 原图 · (1 − m) + 处理后 · m   （m 为归一化蒙版值）
    static QImage blendWithSelection(const QImage &before,
                                     const QImage &after,
                                     const Selection &selection);

    // 上者的底层实现：直接吃"蒙版 + 蒙版有效范围"。
    //
    // ★ 为什么要拆出来 ★
    //   调整对话框的预览是降采样的，它的蒙版也必须缩到同一尺寸 ——
    //   那时手上只有一张 QImage 蒙版，没有 Selection 对象可用。
    //   拆出这一层后，预览与最终提交共用同一段混合代码，
    //   "预览里有选区效果、确定后没有"这类不一致就不可能发生。
    static QImage blendByMask(const QImage &before,
                              const QImage &after,
                              const QImage &mask,
                              const QRect &maskBounds);

    // ── 调整对话框的状态 ──

    // 打开对话框那一刻的图层快照（全尺寸）。它同时是：
    //   · 最终提交时 apply() 的输入
    //   · ImageCommand 的 "before"
    //   · 直方图统计的对象（对话框构造时已经拿去用）
    QImage m_adjustBase;

    // 上面的降采样副本，仅用于预览。见 runAdjustment() 的说明。
    QImage m_adjustPreviewBase;

    // 选区蒙版缩到预览尺寸后的副本 + 它的有效范围。
    // 在 runAdjustment() 里算一次即可：对话框是模态的，
    // 期间选区不可能变化。放进每帧的预览路径会白白多花几毫秒。
    QImage m_adjustPreviewMask;
    QRect  m_adjustPreviewMaskBounds;

    // 当前打开的调整对话框（用于回调，不持所有权 —— 由 runAdjustment 释放）
    AdjustmentDialog *m_adjustDialog = nullptr;

    // 提交时写进历史面板的操作名（"曲线"、"亮度/对比度"…）
    QString m_adjustText;

    // ── 旋转对话框的状态 ──

    // 打开对话框那一刻的降采样合成图。它同时是：
    //   · 预览时 apply() 的输入
    //   · 对话框自己拿去做参数初值（现在没用上，但同一份图不必拷两次）
    //
    // ★ 这里不需要"全尺寸基准图" ★
    //   调整类需要它是因为收尾要 apply() 出全尺寸像素再替换图层；
    //   旋转的收尾走 Document::rotateImageArbitrary()，Document 内部就是
    //   全尺寸运算。所以传全尺寸图进来纯属白占几十 MB。
    QImage m_rotatePreviewBase;

    // 当前打开的旋转对话框（不持所有权 —— 由 openRotateArbitrary 释放）
    RotateImageDialog *m_rotateDialog = nullptr;

    // 预览用的降采样最长边。
    //
    // 1024 的依据：约 100 万像素的单线程 LUT 变换 3~5ms，
    // 加两次格式转换总共 6~10ms，正好落在 60fps 的 16ms 预算内。
    // 取更大的值（如 2048 → 400 万像素）会让预览掉到 20fps 左右，
    // 滑块会有肉眼可见的滞后（见 docs/曲线调整设计.md 5.4）。
    static constexpr int kAdjustPreviewMaxSide = 1024;

    // ── 当前颜色 ──
    // 保存在 MainWindow 而不是 CanvasView 里，原因：
    //   · 颜色是"应用级"状态，不随文档切换而重置
    //   · 多个地方要读它（工具箱指示器、颜色面板、将来的画笔选项条）
    //   · 将来接 QSettings 持久化时只需要一处
    QColor m_foregroundColor = Qt::black;
    QColor m_backgroundColor = Qt::white;
};

} // namespace zl
