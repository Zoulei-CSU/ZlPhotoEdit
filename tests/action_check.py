# -*- coding: utf-8 -*-
# ============================================================================
#  action_check.py —— 主窗口「菜单动作对账」（静态检查，不编译）
# ----------------------------------------------------------------------------
#  为什么需要它：
#
#    ★ 真事 ★ 曾出现 imageMenu->addAction(m_actionRotateArbitrary)
#      而**没有** m_actionRotateArbitrary = new QAction(...) 的情况。
#      QMenu::addAction(nullptr) 是合法的、静默什么都不做 —— 于是菜单项
#      凭空消失。编译器不报错（nullptr 是合法实参），离线自检也测不到
#      （ui_smoke_test 根本不链接 MainWindow），双构建 0 错 0 警，
#      只有真正把程序跑起来点开菜单才会发现。
#
#    这类"定义与使用不成对"的错误，靠编译器是抓不到的 —— 只能靠对账。
#    与 .buildlog/pro_check.py（src 文件 ⇄ .pro 列表）同属一个思路：
#    **凡是"两份清单必须一致"的地方，就要有一条自动对账。**
#
#  检查六条：
#    ① 【错误】进了菜单的 m_xxx，必须有 `m_xxx = new ...` 的赋值
#    ② 【错误】被赋值的 QAction，必须有 setObjectName(...)
#       —— 自动化（截图工具、测试）靠 objectName 找动作，没名字就找不到
#    ③ 【警告】MainWindow.h 里声明了 QAction *m_xxx，却从未进任何菜单
#       —— 典型的"写了动作忘挂菜单"
#    ④ 【错误】代码里 `icons::icon("名字")` 用到的名字，必须有
#       resources/icons/<名字>.svg。名字写错一个字母 → 图标**静默消失**
#       （空 QIcon 画出来什么都没有，不报错）—— 与 ① 是同一类错误，
#       只不过一个丢的是菜单项，一个丢的是图标。
#    ⑤ 【错误】resources/resources.qrc 与 resources/icons/ 目录必须一一对应
#       —— 新增图标后忘了重跑 resources/gen_icons.py 就会漏编资源。
#    ⑥ 【警告】画了却没人用的图标（孤儿）
#       —— 图标是资产不是装饰，没人引用的那份要么是漏接线、要么该删。
#
#  用法： python tests/action_check.py
#  退出码：0 = 通过，1 = 有错误
# ============================================================================

import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ACTIONS_CPP = os.path.join(ROOT, "src", "ui", "MainWindowActions.cpp")
MAINWINDOW_H = os.path.join(ROOT, "src", "ui", "MainWindow.h")

errors = []
warnings = []


def io_open(path):
    # 与 read() 同义，只是名字更直白：本脚本里两处都要用
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def read(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


# ★ 必须先去掉注释再匹配 ★
#   否则"被注释掉的 m_x = new QAction(...)"会被当成真的创建，
#   对账脚本就会被自己的一句注释骗过去（实测踩过：验证脚本能否抓住 bug 时，
#   把出问题的那行注释掉，脚本反而报了通过）。
#
#   局限：不区分字符串字面量里的 "//"（本文件里没有这种串）。
#   真要严谨得用词法分析，但为一个对账脚本不值当 —— 这里够用。
def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


src = strip_comments(read(ACTIONS_CPP))

# ── 收集：被放进菜单/菜单栏的名字 ──
# addAction(m_x) / insertAction(..., m_x) / addMenu(m_x) / insertMenu(..., m_x)
added = set()
for pat in (
    r"\baddAction\s*\(\s*(m_[A-Za-z0-9_]+)\s*\)",
    r"\binsertAction\s*\([^,]*,\s*(m_[A-Za-z0-9_]+)\s*\)",
    r"\baddMenu\s*\(\s*(m_[A-Za-z0-9_]+)\s*\)",
    r"\binsertMenu\s*\([^,]*,\s*(m_[A-Za-z0-9_]+)\s*\)",
):
    added.update(re.findall(pat, src))

# ── 收集：被赋值（= new ...）的名字 ──
assigned = set(re.findall(r"\b(m_[A-Za-z0-9_]+)\s*=\s*new\b", src))

# ── 收集：有 setObjectName 的名字 ──
named = set(re.findall(r"\b(m_[A-Za-z0-9_]+)\s*->\s*setObjectName\s*\(", src))

# ── 收集：MainWindow.h 里声明的 QAction* 成员 ──
declared_actions = set(
    re.findall(r"QAction\s*\*\s*(m_[A-Za-z0-9_]+)", strip_comments(read(MAINWINDOW_H)))
)

# ① 进菜单但未创建 —— 本次的真 bug 形态
for name in sorted(added - assigned):
    errors.append(
        "菜单用了 %s，但 MainWindowActions.cpp 里没有 `%s = new ...`\n"
        "        → QMenu::addAction(nullptr) 静默什么都不做，菜单项会凭空消失"
        % (name, name)
    )

# ② 创建了但没 objectName —— 自动化找不到
for name in sorted(declared_actions & assigned - named):
    errors.append(
        "%s 已创建但没有 setObjectName()\n"
        "        → 截图工具/测试靠 objectName 定位动作，没名字就找不到" % name
    )

# ③ 声明了但从未进菜单
for name in sorted(declared_actions - added):
    warnings.append("%s 在 MainWindow.h 里声明了，但从未 addAction 到任何菜单" % name)

# ---------------------------------------------------------------------------
#  ④⑤⑥ 图标对账
# ---------------------------------------------------------------------------
#  ★ 为什么图标也需要对账 ★
#    icons::icon("tool_brsh") 少一个字母 → 返回空 QIcon → 按钮上没有图。
#    编译器、链接器、其它自检全程沉默；只有 icon_test 会报"有图标是空白的"
#    —— 但它报的是"某个图标画不出来"，不会告诉你"是谁引用了不存在的名字"
#    （未渲染的图标根本不会进入它的遍历范围）。
#    所以"引用的名字"与"存在的文件"这两份清单必须单独对账。
SRC_DIR = os.path.join(ROOT, "src")
ICON_DIR = os.path.join(ROOT, "resources", "icons")
QRC_PATH = os.path.join(ROOT, "resources", "resources.qrc")

on_disk = set()
if os.path.isdir(ICON_DIR):
    on_disk = {f[:-4] for f in os.listdir(ICON_DIR) if f.endswith(".svg")}

# ★ 引用分两级，因为两个方向的容错要求完全相反 ★
#
#   · 硬引用 → 参与"代码用了但文件不存在"的**错误**判定。
#     这种误判会让人去改代码，代价高，所以只统计**确实在调图标接口**的地方：
#       icons::icon("x") / icons::pixmap("x") / 映射表里的第二个字段。
#
#   · 软引用 → 只参与"画好了却没人用"的**孤儿警告**判定。
#     这里可以把网撒得大一点：src/ 里任何与某个图标文件同名的字符串字面量
#     都算引用。
#
#  ★ 为什么要分两级（实测踩过）★
#    LayerPanel.cpp 里的眼睛/锁图标是这么画的：
#        drawRowIcon(visible ? "layer_visible" : "layer_hidden", ...)
#    名字是**普通字符串字面量**，不是 icons:: 调用的实参 —— 只扫 icons::icon()
#    的话，这 6 个图标会被误报成"孤儿"。
#    危险的**不是**这条警告本身，而是它给出的错误建议：谁要是照着警告去
#    "清理"这些图标，面板每一行的眼睛和锁就会集体消失 ——
#    而 LayerPanel 的 drawRowIcon 在图标为空时是**静默跳过**的
#    （单元格仍然可用、不报错、不崩溃）。一条误报会直接引出一个静默失效。
referenced_hard = set()
soft_texts = []
for dirpath, _dirnames, filenames in os.walk(SRC_DIR):
    for fn in filenames:
        if not fn.endswith((".cpp", ".h")):
            continue
        text = strip_comments(io_open(os.path.join(dirpath, fn)))
        soft_texts.append(text)
        # icons::icon(QStringLiteral("x")) / icons::pixmap(QStringLiteral("x"))
        for m in re.finditer(
                r'icons::(?:icon|pixmap)\s*\(\s*Q(?:StringLiteral|Latin1String)\s*\("([^"]+)"',
                text):
            referenced_hard.add(m.group(1))

# IconLibrary.cpp 里那张"工具 id → 图标名"的映射表：
#   {"move", "tool_move"} —— 第二个字段同样是图标名，也要计入。
lib_path = os.path.join(SRC_DIR, "ui", "IconLibrary.cpp")
if os.path.exists(lib_path):
    lib = strip_comments(io_open(lib_path))
    for m in re.finditer(r'\{\s*"[a-z_0-9]+"\s*,\s*"([a-z_0-9]+)"\s*\}', lib):
        referenced_hard.add(m.group(1))

# 软引用：src/ 里出现过的任意字符串字面量，只要它的内容正好是某个图标名
referenced_soft = set()
for text in soft_texts:
    for m in re.finditer(r'"([A-Za-z0-9_]+)"', text):
        if m.group(1) in on_disk:
            referenced_soft.add(m.group(1))

referenced = referenced_hard | referenced_soft

for name in sorted(referenced_hard - on_disk):
    errors.append(
        "代码里用了图标 %s，但 resources/icons/%s.svg 不存在" % (name, name)
        + chr(10) + "        → 写错一个字母就会静默失去图标，请核对名字"
    )

for name in sorted(on_disk - referenced):
    warnings.append("%s.svg 画好了但没有任何代码引用（孤儿图标）" % name)

# qrc 与目录必须一一对应（漏跑 gen_icons.py 会漏编资源）
in_qrc = set()
if os.path.exists(QRC_PATH):
    qrc = io_open(QRC_PATH)
    in_qrc = set(re.findall(r'<file>icons/([^<]*)\.svg</file>', qrc))
    for name in sorted(on_disk - in_qrc):
        errors.append(
            "resources/icons/%s.svg 不在 resources.qrc 里"
            + chr(10) + "        → 请重跑 python resources/gen_icons.py"
        )
    for name in sorted(in_qrc - on_disk):
        errors.append(
            "resources.qrc 里引用了不存在的 resources/icons/%s.svg" % name)

# ── 输出 ──
print("=" * 64)
print("  菜单动作对账：src/ui/MainWindowActions.cpp ⇄ MainWindow.h")
print("=" * 64)
print("  进菜单的名字 %d 个 / 已创建 %d 个 / 有 objectName %d 个 / 头文件声明 %d 个"
      % (len(added), len(assigned), len(named), len(declared_actions)))
print("  引用的图标 %d 个 / 磁盘上的图标 %d 个 / qrc 里的图标 %d 个"
      % (len(referenced), len(on_disk), len(in_qrc)))

for w in warnings:
    print("  [警告] %s" % w)

if errors:
    print("")
    for e in errors:
        print("  [错误] %s" % e)
    print("")
    print("  ✗ 发现 %d 处问题" % len(errors))
    sys.exit(1)

print("")
print("  ✓ 菜单动作与图标全部对账一致（%d 个动作 / %d 个图标）"
      % (len(added), len(referenced)))
sys.exit(0)
