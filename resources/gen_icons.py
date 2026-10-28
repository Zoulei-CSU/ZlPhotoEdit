#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
#  gen_icons.py —— 生成 resources/icons/*.svg 与 resources/resources.qrc
# ----------------------------------------------------------------------------
#  为什么用脚本生成、而不是手写几十个 SVG 文件：
#
#    ① **一套图标必须"看起来是一套"**：统一的 24×24 视框、统一的描边宽度、
#       统一圆角端点、统一颜色。手写迟早会有一个文件忘了改 stroke-width，
#       而那种"就这一个有点不一样"的偏差肉眼极难排查，只有并排摆开才看得出来。
#       把公共属性放在模板里，就不可能写歪。
#    ② 有些记号要用循环算坐标（棋盘格、太阳光芒、直方图柱），手算容易错，
#       写成循环一眼能看懂意图。
#    ③ 改一次主色只需改 STROKE 一处。
#
#  设计约定（新增图标请遵守）：
#    · 视框 24×24，内容留出约 2.5px 的呼吸边距（坐标大致落在 3~21）
#    · 描边 #33393f、宽 1.7、圆头圆角，一律"线描"风格（fill=none）
#    · 需要"面"的地方用 TINT（同色 16% 透明）压一层，别用第二种颜色 ——
#      单色图标在小尺寸下最干净，也不会和主题色打架
#    · 同一个图标最多 5 个图元，24px 下再多就糊成一团
#
#  ★★★ 必须单色：这里写的 #33393f 在运行时会**被整块替换掉** ★★★
#    ui/IconLibrary.cpp 渲染完立刻用 CompositionMode_SourceIn 把整张图
#    刷成当前 palette 的前景色（深色主题下自动变浅），于是同一套 SVG
#    在深浅两主题下都清楚，不用维护第二套图。
#
#    代价只有一个，但它是硬性的：**SVG 里只能有一种颜色**。
#    SourceIn 只看 alpha、把原色全部丢掉 —— 你若在这里写了第二种颜色
#    （例如给警告图标加个红点），运行时会发现红点长成了前景色，
#    而界面看起来完全正常，没有人会想到是配色被吃掉。
#
#    tests/icon_test.cpp 里有一条"每个图标都是单色"的断言守着它，
#    且它是**直接渲染原始 SVG** 判的（上色后的图必然单色，判不出来）。
#    真要保留双色，就得把这个图标排除在上色之外。
#
#    为什么 STROKE 仍写成 #33393f：它是**浅色主题**下的正确前景色，
#    跑 icon_test 的接触表、以及任何拿 SVG 原图直接显示的地方都用这个值。
#
#  用法： python resources/gen_icons.py      （幂等，可反复运行）
# ============================================================================

import os
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ICON_DIR = os.path.join(HERE, "icons")
QRC_PATH = os.path.join(HERE, "resources.qrc")

# ---------------------------------------------------------------------------
#  固定资源（不由本脚本生成、但必须写进 qrc 的文件）
# ---------------------------------------------------------------------------
#  ★ 为什么必须在这里列出来 ★
#
#    resources.qrc 是**整体重写**的（下面用 open(QRC_PATH, "w") 一次写到底）。
#    所以任何"手工往 qrc 里加一行"的做法都活不过下一次运行本脚本 ——
#    行会凭空消失，而且是静默的：qrc 看起来正常，只是少了那一行，
#    直到某个图标变成空白才有人发现。
#
#    因此：**凡是需要进 qrc 的文件，要么由本脚本生成，要么登记在这张表里。**
#
#  路径规则（与 prefix="/" 拼接后得到的运行时路径）：
#    "logo.png"  →  ":/logo.png"        ← 应用图标（AppInfo.cpp 用它）
FIXED_RESOURCES = [
    "logo.png",
]

STROKE = "#33393f"          # 主色（与浅色主题的下拉/菜单文字同色系）
WIDTH = 1.7                 # 描边宽度
TINT = "fill=\"%s\" fill-opacity=\"0.16\"" % STROKE   # 淡淡的"面"
SOLID = "fill=\"%s\"" % STROKE                        # 实心"面"


# ---------------------------------------------------------------------------
#  图元构造小工具（返回 SVG 片段字符串）
# ---------------------------------------------------------------------------
def _attrs(fill=None, dash=None, extra=""):
    a = ""
    if fill is not None:
        a += " " + fill
    if dash is not None:
        a += ' stroke-dasharray="%s"' % dash
    if extra:
        a += " " + extra
    return a


def line(x1, y1, x2, y2, **kw):
    return '<path d="M%g %gL%g %g"%s/>' % (x1, y1, x2, y2, _attrs(**kw))


def path(d, **kw):
    return '<path d="%s"%s/>' % (d, _attrs(**kw))


def rect(x, y, w, h, rx=0, **kw):
    r = ' rx="%g"' % rx if rx else ""
    return '<rect x="%g" y="%g" width="%g" height="%g"%s%s/>' % (
        x, y, w, h, r, _attrs(**kw))


def circle(cx, cy, r, **kw):
    return '<circle cx="%g" cy="%g" r="%g"%s/>' % (cx, cy, r, _attrs(**kw))


def ellipse(cx, cy, rx, ry, **kw):
    return '<ellipse cx="%g" cy="%g" rx="%g" ry="%g"%s/>' % (
        cx, cy, rx, ry, _attrs(**kw))


def sun_rays(cx, cy, r0, r1, n=8, start=-90.0):
    """一圈光芒（调色板图标 / 亮度图标都用得上）"""
    import math
    out = []
    for i in range(n):
        a = math.radians(start + 360.0 * i / n)
        out.append(line(round(cx + r0 * math.cos(a), 2),
                        round(cy + r0 * math.sin(a), 2),
                        round(cx + r1 * math.cos(a), 2),
                        round(cy + r1 * math.sin(a), 2)))
    return "".join(out)


def checkerboard(x, y, size, cells):
    """棋盘格（透明背景图标）；深色格填实心"""
    out = []
    cell = size / float(cells)
    for j in range(cells):
        for i in range(cells):
            if (i + j) % 2 == 0:
                out.append(rect(round(x + i * cell, 3), round(y + j * cell, 3),
                                round(cell, 3), round(cell, 3), fill=SOLID))
    return "".join(out)


# ---------------------------------------------------------------------------
#  ★ 图标表 ★
#    键名 = 文件名 = `icons::icon("键名")` 里的那个字符串。
#    tests/action_check.py 会核对"代码里引用的名字"与"实际存在的 .svg"成对，
#    写错名字（少一个字母）会立刻被抓住，而不是等看到界面上图标不见了。
# ---------------------------------------------------------------------------
DASH = "3 2.5"          # 选框类图标统一用同一种虚线节奏

ICONS = {}

# ── 工具箱（当前 6 个 + Phase 6 要补的 8 个，一次画齐）───────────────
ICONS["tool_move"] = [
    line(12, 3.5, 12, 20.5), line(3.5, 12, 20.5, 12),
    path("M8.6 6.9L12 3.5l3.4 3.4"), path("M8.6 17.1L12 20.5l3.4-3.4"),
    path("M6.9 8.6L3.5 12l3.4 3.4"), path("M17.1 8.6L20.5 12l-3.4 3.4"),
]
ICONS["tool_brush"] = [
    path("M16.6 3.4l4 4-9.3 9.3-4-4z"),
    path("M7.3 12.7L4.2 20.2l7.5-3.1z", fill=TINT),
]
ICONS["tool_eraser"] = [
    #   ★ 只有一块斜方块时，看的人只会读成"菱形/几何图形" ★
    #   加一条台面横线之后才有"一块橡皮摆在桌面上"的意思 ——
    #   这叫给形状一个**参照物**，是画小图标最划算的一招。
    #   分隔线与短边平行，顺带说明它是"双色橡皮"。
    line(3.5, 21.2, 20.5, 21.2),
    path("M4.6 13.9L14 8.5l2.6 4.5-9.4 5.4z"),
    line(8.5, 8.7, 11.1, 13.1),
]
ICONS["tool_rectselect"] = [rect(3.5, 5, 17, 14, rx=0.8, dash=DASH)]
ICONS["tool_ellipseselect"] = [ellipse(12, 12, 8.5, 6.5, dash=DASH)]
ICONS["tool_crop"] = [
    path("M6.5 2.5v15h15"), path("M2.5 6.5h15v15"),
]
ICONS["tool_bucket"] = [
    #   ★ 45° 斜放的桶，在 24px 下只会被读成"斜方块" ★
    #   改成正视：桶口椭圆 + 梯形桶身 + 提手 + 桶边一滴。
    #   （提手和桶口椭圆是关键 —— 少了它们，梯形会被当成"过滤器"。）
    path("M7.7 5.4a3.7 3.7 0 016.7 0"),
    ellipse(11, 7.4, 6.3, 2.2),
    path("M4.7 7.4l1.5 10.6a2.2 2.2 0 002.2 1.9h5.3a2.2 2.2 0 002.2-1.9l1.5-10.6"),
    path("M19.8 13.6c1.1 1.5 1.7 2.5 1.7 3.2a1.7 1.7 0 11-3.4 0c0-.7.6-1.7 1.7-3.2z", fill=TINT),
]
ICONS["tool_eyedropper"] = [
    circle(5.6, 18.4, 2.1, fill=TINT), line(7.2, 16.8, 14.6, 9.4),
    path("M13.4 6.4l4.2 4.2 1.7-1.7a3 3 0 00-4.2-4.2z"),
]
ICONS["tool_line"] = [line(4, 20, 20, 4)]
ICONS["tool_rect"] = [rect(3.5, 5.5, 17, 13, rx=0.8)]
ICONS["tool_ellipse"] = [ellipse(12, 12, 8.5, 6.5)]
ICONS["tool_text"] = [
    path("M4.5 7V4.5h15V7"), line(12, 4.5, 12, 19.5), line(8.5, 19.5, 15.5, 19.5),
]
ICONS["tool_hand"] = [
    path("M7.8 13.2V6.6a1.4 1.4 0 012.8 0v4.4"),
    path("M10.6 11V4.8a1.4 1.4 0 012.8 0v6.2"),
    path("M13.4 11V5.6a1.4 1.4 0 012.8 0v6.6"),
    path("M16.2 13.4V9a1.4 1.4 0 012.8 0v6.4a5.6 5.6 0 01-5.6 5.6h-2.1"
         "a5.6 5.6 0 01-5.6-5.6v-2.8a1.4 1.4 0 012.8 0"),
]
ICONS["tool_zoom"] = [circle(10.5, 10.5, 6.8), line(15.4, 15.4, 20.5, 20.5)]
ICONS["tool_magicwand"] = [
    #   ★ 杖身 + 杖尖的四角星 + 两粒星火 ★
    #   只画一根斜棍会被读成"直线工具"；只画一颗星会被读成
    #   "收藏 / 评分"。两者合在一起才立刻读成"魔杖"：
    #   斜棍给出"工具"的身份，星星给出"按颜色魔法选取"的语义。
    #   两粒小星火是最后一笔 —— 它们负责说明"这东西在发光"，
    #   同时也是在 24px 下把它与 tool_line 区分开的关键
    #   （直线工具没有这两个小十字）。
    line(4.4, 19.6, 14.8, 9.2),
    path("M16.6 3.2l1.1 3.1 3.1 1.1-3.1 1.1-1.1 3.1-1.1-3.1-3.1-1.1 3.1-1.1z", fill=TINT),
    line(19.6, 14.8, 19.6, 18.8), line(17.6, 16.8, 21.6, 16.8),
    line(7.0, 5.0, 7.0, 8.2), line(5.4, 6.6, 8.6, 6.6),
]

# ── 文件 ────────────────────────────────────────────────────────────
ICONS["file_new"] = [
    path("M6.5 3.5h7L18 8v12.5H6.5z"), path("M13.5 3.5V8H18"),
    line(12, 11.5, 12, 17.5), line(9, 14.5, 15, 14.5),
]
ICONS["file_open"] = [
    path("M3.5 18.8V6.6h5.7l2 2.6h9.3v9.6z"),
    line(12, 11.4, 12, 17), path("M9.6 13.8L12 11.4l2.4 2.4"),
]
# 「打开为图层」：沿用 file_* 家族的"文件夹"记号（表示"从文件来"），
# 加上一枚从上方落进去的图层纸片。与 file_open（文件夹内上箭头）和
# layer_new（两枚纸片 + 加号）都不会看混。
ICONS["file_open_as_layer"] = [
    path("M2.6 19.8V9.2h5.2l1.8 2.3h9.9v8.3z"),
    path("M12 2.6l6.2 3.1L12 8.8 5.8 5.7z"),
    line(12, 8.8, 12, 11.2), path("M10.5 10L12 11.5 13.5 10"),
]
ICONS["file_save"] = [
    path("M4.5 4.5h11.2L20 8.8v10.7H4.5z"), path("M8 4.5v5h6.5v-5"),
    path("M7.5 19.5v-5.5h9v5.5"),
]
ICONS["file_save_as"] = [
    path("M3.5 4.5h9.6L17 8.2v5.6"), path("M3.5 4.5v15h6.5"),
    path("M6.8 4.5v4h5.4v-4"), path("M6.5 19.5v-4.6h3.5"),
    path("M13.9 15.9l4.7-4.7 2.3 2.3-4.7 4.7-3 .7z"),
]
ICONS["file_close"] = [
    rect(4.5, 5.5, 15, 13, rx=1.5),
    line(9.5, 9.5, 14.5, 14.5), line(14.5, 9.5, 9.5, 14.5),
]
ICONS["file_quit"] = [
    path("M13.5 4.5h-9v15h9"), line(10, 12, 20.5, 12),
    path("M17.5 8.5L21 12l-3.5 3.5"),
]
ICONS["file_print"] = [
    #   顶上的进纸 + 机身 + 从出纸口露出来的一张纸。
    #   指示灯那一点是"打印机"与"一个方盒子"的唯一区别。
    path("M7 3.5h10v4.5H7z"),
    rect(3.5, 8, 17, 8.5, rx=1.5),
    path("M7 16.5h10V21H7z"),
    circle(6.6, 11.3, 0.9, fill=SOLID),
]
ICONS["file_export_pdf"] = [
    #   与 file_new 同族的折角纸，加一枚向下的箭头表示往外送。
    path("M6.5 3.5h7L18 8v12.5H6.5z"), path("M13.5 3.5V8H18"),
    line(12, 11.5, 12, 17), path("M9.8 14.8L12 17l2.2-2.2"),
]

# ── 编辑 ────────────────────────────────────────────────────────────
ICONS["edit_undo"] = [
    # ★ 几何要居中在 24×24 视框里 ★
    #   原来这两条曲线整体偏在 (13.5, 10.5)，视野中心是 (12, 12) ——
    #   16px 档只差 1px 看不出来，24/48 档（尤其 Retina 的 2×）上偏移被
    #   放大成"图标贴右上角"，正是工具栏里撤销/重做显得没居中的原因。
    #   所有点统一平移 (-1.5, +1.5)。
    path("M7 9H12.5a4.5 4.5 0 010 9H7.5"), path("M10 6l-3 3 3 3"),
]
ICONS["edit_redo"] = [
    # 与 undo 对称：统一平移 (+1.5, +1.5)
    path("M17 9H11.5a4.5 4.5 0 000 9H17"), path("M14 6l3 3-3 3"),
]
ICONS["edit_cut"] = [
    circle(6.6, 18.4, 2.3), circle(17.4, 18.4, 2.3),
    line(8.1, 16.9, 18.6, 5.6), line(15.9, 16.9, 5.4, 5.6),
]
ICONS["edit_copy"] = [
    rect(8.5, 8.5, 11, 11, rx=1.5),
    path("M15.5 5.5h-9a1.5 1.5 0 00-1.5 1.5v9"),
]
ICONS["edit_paste"] = [
    rect(6.5, 5.5, 11, 15, rx=1.5), rect(9.5, 3.5, 5, 3.5, rx=1, fill=TINT),
    line(9.5, 12, 14.5, 12), line(9.5, 16, 14.5, 16),
]
ICONS["edit_paste_into"] = [
    rect(6.5, 5.5, 11, 15, rx=1.5), rect(9.5, 3.5, 5, 3.5, rx=1, fill=TINT),
    line(12, 10.5, 12, 15.5), path("M9.8 13.3L12 15.5l2.2-2.2"),
]
ICONS["edit_delete"] = [
    line(4.8, 7, 19.2, 7), path("M9.2 7V4.6h5.6V7"),
    path("M6.6 7l1 13h8.8l1-13"),
    line(10.3, 10.6, 10.3, 16.6), line(13.7, 10.6, 13.7, 16.6),
]
ICONS["edit_undo_limit"] = [
    #   三层叠起来的历史记录，最上面一条用 TINT 压住 ——
    #   表示"可回溯的深度"，与时钟（menu_recent）区分开。
    rect(4.5, 4.2, 15, 3.4, rx=0.9, fill=TINT),
    rect(4.5, 10.3, 15, 3.4, rx=0.9),
    rect(4.5, 16.4, 15, 3.4, rx=0.9),
]

# ── 选区 ────────────────────────────────────────────────────────────
ICONS["select_all"] = [
    rect(3.5, 4.5, 17, 15, rx=0.8, dash=DASH),
    path("M8 12.2l2.8 2.8 5.4-5.6"),
]
ICONS["select_none"] = [
    rect(3.5, 4.5, 17, 15, rx=0.8, dash=DASH),
    line(6.5, 17.5, 17.5, 6.5),
]
ICONS["select_invert"] = [
    rect(4.5, 4.5, 10, 10, rx=1), rect(9.5, 9.5, 10, 10, rx=1, fill=TINT),
]

# ── 图像调整 / 滤镜 ──────────────────────────────────────────────────
ICONS["adjust_brightness"] = [
    circle(12, 12, 4.3), sun_rays(12, 12, 6.5, 9.1),
]
ICONS["adjust_hue"] = [
    circle(12, 12, 8.6), circle(12, 12, 3.4),
    line(12, 3.4, 12, 8.6), line(4.6, 16.3, 9.1, 13.7),
    line(19.4, 16.3, 14.9, 13.7),
]
ICONS["adjust_curves"] = [
    rect(3.5, 3.5, 17, 17, rx=1.2),
    path("M6 17.5c4.6 0 5.4-11 12-11"),
    rect(4.7, 16.2, 2.6, 2.6, rx=0.5, fill=TINT),
    rect(16.7, 5.2, 2.6, 2.6, rx=0.5, fill=TINT),
]
ICONS["adjust_levels"] = [
    line(3.5, 19.8, 20.5, 19.8),
    rect(5.0, 13.8, 2.2, 6, fill=TINT), rect(8.0, 9.8, 2.2, 10, fill=TINT),
    rect(11.0, 5.8, 2.2, 14, fill=TINT), rect(14.0, 11.3, 2.2, 8.5, fill=TINT),
    rect(17.0, 15.3, 2.2, 4.5, fill=TINT),
]
ICONS["adjust_grayscale"] = [
    circle(12, 12, 8.6),
    path("M12 3.4a8.6 8.6 0 010 17.2z", fill=SOLID),
]
ICONS["adjust_invert"] = [
    rect(3.6, 3.6, 16.8, 16.8, rx=1.5),
    path("M4.5 4.5h15v15z", fill=SOLID),
]

# ── 几何变换 ────────────────────────────────────────────────────────
ICONS["rotate_cw"] = [
    path("M19.5 12A7.5 7.5 0 1 1 12 4.5"),
    path("M9.4 2.9L12 4.5l-2.6 1.6"),
]
ICONS["rotate_ccw"] = [
    path("M4.5 12A7.5 7.5 0 1 0 12 4.5"),
    path("M14.6 2.9L12 4.5l2.6 1.6"),
]
ICONS["rotate_180"] = [
    circle(12, 12, 7.5, dash=DASH),
    path("M9.4 2.9L12 4.5l-2.6 1.6"),
    path("M14.6 17.9L12 19.5l2.6 1.6"),
]
ICONS["rotate_arbitrary"] = [
    path("M12 4.5A7.5 7.5 0 1 1 4.5 12"),
    path("M6.4 14.6L4.5 12l-1.9 2.6"),
    path("M12 12V4.5", dash="2 2.5"),
]
# ★ 翻转：靠"一深一浅 + 轴的方向"在 16px 下站住 ★
#   原来的写法是"轴 + 两个同样 TINT 的三角形"，24px 以上没问题，
#   但菜单里的图标是 16px —— 两个浅三角形糊成一团，轴又被盖住，
#   结果 flip_h 和 flip_v 长得一模一样（菜单截图里一眼看出来的）。
#   改法有两条，缺一不可：
#     ① 三角形放大到几乎顶满（6.5 宽 → 7 宽），16px 下才够厚；
#     ② 一深一浅（左/上是 SOLID，右/下是 TINT）——
#        深浅对比是在小尺寸下区分"哪边是原图"的唯一有效线索，
#        同时让两个形状即使糊在一起也不对称，不会再被看成同一个图标。
ICONS["flip_h"] = [
    path("M12 2.5v19", dash="2.5 2.5"),
    path("M10 5.5L3.5 12l6.5 6.5z", fill=SOLID),
    path("M14 5.5L20.5 12l-6.5 6.5z", fill=TINT),
]
ICONS["flip_v"] = [
    path("M2.5 12h19", dash="2.5 2.5"),
    path("M5.5 10L12 3.5l6.5 6.5z", fill=SOLID),
    path("M5.5 14L12 20.5l6.5-6.5z", fill=TINT),
]
ICONS["image_size"] = [
    rect(3.5, 5.5, 17, 13, rx=1.5),
    line(8, 12, 16, 12),
    path("M9.8 10.2L8 12l1.8 1.8"), path("M14.2 10.2L16 12l-1.8 1.8"),
]
ICONS["canvas_size"] = [
    rect(3.5, 3.5, 17, 17, rx=0.8, dash=DASH),
    rect(7.5, 7.5, 9, 9, rx=1),
]

# ── 图层 ────────────────────────────────────────────────────────────
_SHEET = path("M12 3.6l8.2 4.4L12 12.4 3.8 8z")
_SHEET2 = path("M4 12.4l8 4.4 8-4.4")
ICONS["layer_new"] = [
    _SHEET, _SHEET2,
    line(18, 15.6, 18, 21), line(15.3, 18.3, 20.7, 18.3),
]
ICONS["layer_duplicate"] = [
    path("M12 3.4l7.8 4.2-7.8 4.2-7.8-4.2z"),
    path("M4.2 12.6l7.8 4.2 7.8-4.2", dash="2.6 2.4"),
]
ICONS["layer_delete"] = list(ICONS["edit_delete"])      # 别名：垃圾桶（语义最清楚）
ICONS["layer_up"] = [
    line(4.5, 20.5, 19.5, 20.5),
    line(12, 4, 12, 16.5), path("M7.6 8.4L12 4l4.4 4.4"),
]
ICONS["layer_down"] = [
    line(4.5, 3.5, 19.5, 3.5),
    line(12, 7.5, 12, 20), path("M7.6 15.6L12 20l4.4-4.4"),
]
ICONS["layer_merge_down"] = [
    path("M12 3.2l7.2 3.8L12 10.8 4.8 7z"),
    line(12, 12.5, 12, 17.4), path("M9.2 14.6L12 17.4l2.8-2.8"),
    path("M4.8 20.6h14.4"),
]
ICONS["layer_flatten"] = [
    line(12, 3.6, 12, 10.4), path("M9.2 7.6L12 10.4l2.8-2.8"),
    rect(3.5, 13.5, 17, 7, rx=1.5),
]

# ── 图层面板：行内状态（睁眼 / 闭眼 / 锁）────────────────────────────
#
#  这三个是**画在列表行里**的小图标，实际尺寸只有 14~16px，
#  比菜单图标更小。所以图元数必须压到最少（眼 = 轮廓 + 瞳孔），
#  并且"开 / 关"两态要在**形状**上就分得开，不能只靠颜色 ——
#  单色图标没有颜色可依赖，斜杠是唯一可靠的"否定"记号。
_EYE = path("M2.9 12c3.3-4.7 7-7 9.1-7s5.8 2.3 9.1 7c-3.3 4.7-7 7-9.1 7"
            "s-5.8-2.3-9.1-7z")
ICONS["layer_visible"] = [
    _EYE,
    circle(12, 12, 2.9),
]
ICONS["layer_hidden"] = [
    _EYE,
    circle(12, 12, 2.9),
    line(4.4, 19.6, 19.6, 4.4),      # 斜杠：唯一的"否定"记号
]
# 锁的两种状态：**梁的形状不同** —— 关着的锁梁落回锁体右侧，开着的锁梁
# 只从左侧立起来、右端悬空。靠"梁有没有搭上"区分，比加斜杠更直观。
_LOCK_BODY = rect(5.2, 10.4, 13.6, 9.8, rx=1.7)
_LOCK_HOLE = line(12, 14.2, 12, 16.6)
ICONS["layer_lock"] = [
    _LOCK_BODY,
    path("M8.6 10.4V7.9a3.4 3.4 0 0 1 6.8 0v2.5"),
    _LOCK_HOLE,
]
ICONS["layer_unlock"] = [
    _LOCK_BODY,
    path("M8.6 10.4V7.9a3.4 3.4 0 0 1 6.8 0"),
    _LOCK_HOLE,
]

# ── 图层蒙版 ────────────────────────────────────────────────────────
#   左边方块＝图层，右边圆＝蒙版，两者略微叠住表示"挂在这一层上"。
#   加号 / 叉画在圆里，语义与"添加/删除蒙版"直接对应。
_MASK_LAYER_BOX = rect(3.2, 5.2, 11.4, 13.6, rx=1)
_MASK_CIRCLE    = circle(16.8, 12, 5.2, fill=TINT)
ICONS["layer_mask_add"] = [
    _MASK_LAYER_BOX, _MASK_CIRCLE,
    line(16.8, 9.4, 16.8, 14.6), line(14.2, 12, 19.4, 12),
]
ICONS["layer_mask_remove"] = [
    _MASK_LAYER_BOX, _MASK_CIRCLE,
    line(14.9, 10.1, 18.7, 13.9), line(18.7, 10.1, 14.9, 13.9),
]

# ── 图层面板：属性区的两个小标题（混合模式 / 不透明度）──────────────
#
#  这两个只作为"标签的图标"出现在下拉框与滑块前面，
#  语义必须一眼可辨：
#    混合 = 两个圆相交（相交处压一层淡色）
#    不透明 = 一个方块左半边填实 —— 这是"透明度"最通用的记号，
#            左边"满"右边"空"，不用画渐变也能看懂
ICONS["layer_blend"] = [
    circle(9.3, 12, 6.1),
    circle(14.7, 12, 6.1),
    # 两圆相交的"透镜"区域：两段反向圆弧闭合成形。
    # 半径与圆心距决定交点必然落在 x=12、y=12±5.6 处，
    # 与上面两个圆的半径 6.1 一致（6.1² - 2.7² = 5.6²）。
    path("M12 6.4A6.1 6.1 0 0 1 12 17.6A6.1 6.1 0 0 0 12 6.4Z", fill=TINT),
]
ICONS["layer_opacity"] = [
    rect(3.8, 3.8, 16.4, 16.4, rx=2.2),
    path("M3.8 6a2.2 2.2 0 0 1 2.2-2.2H12v16.4H6A2.2 2.2 0 0 1 3.8 18z",
         fill=SOLID),
]

# ── 合并可见图层 ────────────────────────────────────────────────────
#
#  ★ 必须和 layer_merge_down 在 16px 下分得开 ★
#    merge_down 是"一片层 + 向下箭头 + 底线"，
#    所以这里**不画底线**，改用眼睛（= 可见）压在上半部、
#    箭头收在下方，两者靠"上半部是平行四边形还是眼睛"区分。
ICONS["layer_merge_visible"] = [
    path("M3.1 9.4c3-4.3 6.4-6.4 8.9-6.4s5.9 2.1 8.9 6.4c-3 4.3-6.4 6.4"
         "-8.9 6.4s-5.9-2.1-8.9-6.4z"),
    circle(12, 9.4, 2.5),
    line(12, 16.6, 12, 21.2), path("M9.4 18.6L12 21.2l2.6-2.6"),
]

# ── 重命名图层 ──────────────────────────────────────────────────────
#
#  "一片层 + 两条名字 + 一支斜笔"。
#
#  ★ 为什么是"斜笔"而不是"文字 A" ★
#    A 只有三条笔画，缩到 16px 会糊成一个实心点，与 tool_text 也撞形。
#    斜笔的走向是 45°，和全部图标的横竖笔画都不共线 ——
#    在最小档位下它是唯一一个"斜着的东西"，一眼就能认出来。
ICONS["layer_rename"] = [
    rect(3.6, 4.2, 16.4, 15.2, rx=2.2),
    line(7.2, 9.2, 14.6, 9.2),
    line(7.2, 13.2, 11.6, 13.2),
    line(14.2, 19.4, 20.6, 13.0),
    path("M13.2 20.4l1-3.2 2.2 2.2z", fill=SOLID),
]

# ── 视图 ────────────────────────────────────────────────────────────
_GLASS = circle(10.5, 10.5, 6.8)
_HANDLE = line(15.4, 15.4, 20.5, 20.5)
ICONS["view_zoom_in"] = [
    _GLASS, _HANDLE, line(10.5, 7.6, 10.5, 13.4), line(7.6, 10.5, 13.4, 10.5),
]
ICONS["view_zoom_out"] = [_GLASS, _HANDLE, line(7.6, 10.5, 13.4, 10.5)]
ICONS["view_zoom_fit"] = [
    path("M3.5 8.5v-5h5"), path("M15.5 3.5h5v5"),
    path("M20.5 15.5v5h-5"), path("M8.5 20.5h-5v-5"),
    rect(8.5, 8.5, 7, 7, rx=1),
]
ICONS["view_zoom_100"] = [
    rect(3.5, 3.5, 17, 17, rx=1.5),
    line(8, 8.6, 8, 15.4), line(16, 8.6, 16, 15.4),
    circle(12, 10.4, 1.05, fill=SOLID), circle(12, 13.6, 1.05, fill=SOLID),
]
ICONS["view_grid"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(9.17, 3.5, 9.17, 20.5), line(14.83, 3.5, 14.83, 20.5),
    line(3.5, 9.17, 20.5, 9.17), line(3.5, 14.83, 20.5, 14.83),
]
ICONS["view_checkerboard"] = [
    rect(3.5, 3.5, 17, 17, rx=1), checkerboard(3.5, 3.5, 17, 4),
]
ICONS["view_fullscreen"] = [
    #   四角向外张的对角线。与 view_zoom_fit 的"四角向内收"正好是一对，
    #   但两者都是拐角折线时太像，所以这里再加指向角尖的对角线，
    #   让"向外扩"一眼可辨。
    path("M9.2 3.5H3.5v5.7"), path("M14.8 3.5h5.7v5.7"),
    path("M20.5 14.8v5.7h-5.7"), path("M3.5 14.8v5.7h5.7"),
    path("M3.5 3.5L7.4 7.4"), path("M20.5 3.5L16.6 7.4"),
    path("M20.5 20.5L16.6 16.6"), path("M3.5 20.5L7.4 16.6"),
]

# ── 标尺与参考线（阶段七）──────────────────────────────────────────
#   一套四张，视觉上必须"一眼看出是同族"：同一张画布方框，
#   区别只在框里是"尺子"还是"参考线"、以及右下角有没有锁/叉。
ICONS["view_ruler"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(3.5, 8.5, 20.5, 8.5),                      # 尺子与画布的分界
    line(6.5, 3.5, 6.5, 6.6), line(9.5, 3.5, 9.5, 5.4),
    line(12.5, 3.5, 12.5, 6.6), line(15.5, 3.5, 15.5, 5.4),
    line(18.5, 3.5, 18.5, 6.6),                     # 长短相间的刻度
]
ICONS["view_guides"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(3.5, 10.5, 20.5, 10.5, dash=DASH),
    line(13.5, 3.5, 13.5, 20.5, dash=DASH),
]
ICONS["view_guides_lock"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(3.5, 9.5, 20.5, 9.5, dash=DASH),
    path("M9.8 12.6v-1.4a2.2 2.2 0 0 1 4.4 0v1.4"),   # 锁梁
    rect(8.6, 12.6, 6.8, 4.4, rx=1.0),                 # 锁体
]
ICONS["view_guides_clear"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(3.5, 9.5, 20.5, 9.5, dash=DASH),
    line(13.5, 3.5, 13.5, 20.5, dash=DASH),
    line(15.8, 15.8, 19.6, 19.6), line(19.6, 15.8, 15.8, 19.6),   # 叉
]

# ── 网格的间距与吸附（阶段七收尾）────────────────────────────────────
#   与 view_grid 同族：同一张画布方框。区别在框里的记号 ——
#   间距用"两条竖线 + 中间一支双向箭头"（在说"这两条线之间有多宽"），
#   吸附用"一个方块正落在网格交点上"（在说"东西被吸到格子上了"）。
ICONS["view_grid_spacing"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(8.5, 3.5, 8.5, 14.5), line(15.5, 3.5, 15.5, 14.5),       # 两条被测量的竖线
    # 双向箭头：←→ 之间的宽度就是"间距"
    path("M8.5 17.6h7M8.5 17.6l2.2-1.6M8.5 17.6l2.2 1.6"
         "M15.5 17.6l-2.2-1.6M15.5 17.6l-2.2 1.6"),
]
ICONS["view_grid_snap"] = [
    rect(3.5, 3.5, 17, 17, rx=1),
    line(11.5, 3.5, 11.5, 20.5), line(3.5, 11.5, 20.5, 11.5),     # 一条竖线 + 一条横线
    rect(7.5, 7.5, 8, 8, rx=1.2, fill=SOLID),                     # 方块正卡在交点上
]

# ── 颜色 ────────────────────────────────────────────────────────────
ICONS["color_foreground"] = [
    rect(3.5, 3.5, 11, 11, rx=1.5), rect(9.5, 9.5, 11, 11, rx=1.5, fill=SOLID),
]
ICONS["color_background"] = [
    rect(3.5, 3.5, 11, 11, rx=1.5, fill=TINT), rect(9.5, 9.5, 11, 11, rx=1.5),
]
ICONS["color_swap"] = [
    #   一对双向箭头就够了。曾经是"两个色块 + 两条细箭头"，
    #   24px 下三个元素互相挤，接触表上看着是一坨；去掉色块反而清楚。
    path("M4 8.6h13.2"), path("M15 6.5L17.2 8.6 15 10.7"),
    path("M20 15.4H6.8"), path("M9 13.3L6.8 15.4 9 17.5"),
]
ICONS["color_reset"] = [
    path("M20 17.6a8.2 8.2 0 11-2.5-10.5"),
    path("M17.4 4.4l.7 3.7-3.7.6"),
]

# ── 菜单（子菜单项） ────────────────────────────────────────────────
#   子菜单也是 QAction，QMenu::addMenu() 返回的就是那个动作 —— 它一样能挂图标。
#   实测「最近打开」和「面板」这两项当时是全菜单里仅有的两个"没有图标"的条目，
#   在展开的菜单里一眼就能看出不齐。补上之后 48 个菜单条目全部有图。
ICONS["menu_recent"] = [
    #   时钟：'最近'的通用隐喻（比"一张纸 + 时钟"在 16px 下清楚得多）
    circle(12, 12, 8.4),
    line(12, 7.2, 12, 12.2), line(12, 12.2, 15.9, 14.3),
]
ICONS["menu_panels"] = [
    #   左侧主区 + 右侧面板：右边一块用 TINT 压一下，否则 16px 下
    #   只剩一个方框加一条竖线，看不出"面板"的意思
    rect(3.5, 4.5, 17, 15, rx=1.5),
    rect(14.2, 4.5, 6.3, 15, rx=1.5, fill=TINT),
    line(14.2, 4.5, 14.2, 19.5),
]

# ── 屏幕截图（「文件 → 屏幕截图」）────────────────────────────────────
#   三个图标各回答一个问题：
#     子菜单 —— 这是干什么的      立即项 —— 怎么触发
#     延迟项 —— 什么时候触发
ICONS["menu_screen_capture"] = [
    #   显示器 + 底座 + 里面被"截走"的那一块（TINT 压一层）
    #   底座那条横线是这个形状的关键：只有它能把"显示器"与
    #   一个普通方框（view_guides / view_grid 那种）区分开。
    rect(3.2, 4.4, 17.6, 13.0, rx=1.5),
    rect(6.8, 7.4, 6.4, 6.4, rx=0.9, fill=TINT),
    line(9.4, 20.6, 14.6, 20.6),                    # 底座
    line(12, 17.4, 12, 20.6),                       # 支架
]
ICONS["action_screen_capture"] = [
    #   相机：机身 + 顶部取景凸起 + 镜头
    #   ★ 凸起不能省 ★ 只有机身和镜头的话，16px 下会退化成
    #   "方框里有个圆" —— 与 view_zoom_* 那几个放大镜完全分不开。
    rect(3.2, 7.2, 17.6, 11.6, rx=1.8),
    path("M8.8 7.2L10.3 4.9H13.7L15.2 7.2"),
    circle(12, 13.0, 3.4),
]
ICONS["action_capture_delay"] = [
    #   沙漏 = "等一会儿"
    #   ★ 为什么不用闹钟/时钟 ★ 同一个「文件」菜单里已经有 menu_recent
    #   （时钟 + 指针）了，两个时钟挨着放会互相干扰；沙漏收腰的轮廓
    #   在 16px 下也更容易认出来。
    line(7.2, 4.4, 16.8, 4.4), line(7.2, 19.6, 16.8, 19.6),
    path("M8.6 4.4C8.6 9.2 12 11.0 12 12.0C12 13.0 8.6 14.8 8.6 19.6"),
    path("M15.4 4.4C15.4 9.2 12 11.0 12 12.0C12 13.0 15.4 14.8 15.4 19.6"),
]

# ── 主题（「文件 → 主题」）────────────────────────────────────────────
#   半明半暗的一个圆：左半只有描线、右半压一层 TINT，中间一道竖分界。
#   "同一个东西的两种明暗"正是这个菜单要表达的事。
#   ★ 为什么不画太阳/月亮 ★ 那会读成"亮度调节"，而这个菜单改的是整个界面
#   的配色；且「调整 → 亮度/对比度」已经占用了太阳（带光芒的圆），
#   再来一个太阳会撞。这里没有光芒、有明确的竖直分界，16px 下也分得开。
#
#    ⚠ 与 adjust_grayscale 的第 2 个圆形态接近（都是半实心圆）。两者分别在
#    「文件」与「调整」菜单里、不会并排出现，加上这里有一条贯通的竖线，
#    足够了；将来若要把它们挪进同一个菜单，得给其中一个换形态。
ICONS["menu_theme"] = [
    circle(12, 12, 8.4),
    path("M12 3.6A8.4 8.4 0 0 1 12 20.4Z", fill=TINT),
    line(12, 3.6, 12, 20.4),
]

# ── 帮助 ────────────────────────────────────────────────────────────
ICONS["help_about"] = [
    circle(12, 12, 8.6), line(12, 11, 12, 16.6),
    circle(12, 7.8, 1.15, fill=SOLID),
]
ICONS["help_qt"] = [
    #   圆角方框 + 环 + 穿过右下角的尾巴 —— 这才是 Qt 标志的轮廓。
    #   （曾经只画"环 + 斜杠"，结果和放大镜一模一样，接触表上一眼就撞了。）
    rect(3.6, 3.6, 16.8, 16.8, rx=4.5),
    circle(11.4, 11.4, 4.4), line(13.8, 13.8, 19.5, 19.5),
]


# ---------------------------------------------------------------------------
#  写出
# ---------------------------------------------------------------------------
SVG_TMPL = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
            'fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round" '
            'stroke-linejoin="round">\n  %s\n</svg>\n')


def main():
    if not os.path.isdir(ICON_DIR):
        os.makedirs(ICON_DIR)

    written = []
    for name in sorted(ICONS):
        body = "\n  ".join(ICONS[name])
        svg = SVG_TMPL % (STROKE, WIDTH, body)

        # ★ 生成时就顺手校验 XML ★
        #   SVG 少一个引号，Qt 那边只会"渲染不出来"（图标静默变成空白），
        #   不会报任何错 —— 等到界面上发现时已经离病灶很远了。
        #   在源头用标准库解析一次，代价几乎为零。
        try:
            ET.fromstring(svg)
        except ET.ParseError as e:
            raise SystemExit("图标 %s 的 SVG 不是合法 XML：%s" % (name, e))

        with open(os.path.join(ICON_DIR, name + ".svg"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(svg)
        written.append(name)

    # 顺手清掉已经不再生成的旧文件（改名/删除图标时不留垃圾）
    stale = []
    for fn in sorted(os.listdir(ICON_DIR)):
        if fn.endswith(".svg") and fn[:-4] not in ICONS:
            os.remove(os.path.join(ICON_DIR, fn))
            stale.append(fn[:-4])

    with open(QRC_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("<!-- 由 resources/gen_icons.py 生成，请勿手改 -->\n")
        # ★ prefix 会与下面 <file> 的相对路径**拼接** ★
        #   prefix="/icons" 配 <file>icons/x.svg 会得到
        #   :/icons/icons/x.svg —— 多一层，而且 rcc --list 看不出来。
        #   用 prefix="/" 与 <file> 的 icons/ 合起来才是 :/icons/x.svg。
        f.write("<RCC>\n  <qresource prefix=\"/\">\n")
        # ★ 固定资源写在最前面 ★（见文件头的 FIXED_RESOURCES 说明）
        #   漏写这里的后果是"手工加的 qrc 行被抹掉"，且完全静默。
        for name in FIXED_RESOURCES:
            f.write('    <file>%s</file>\n' % name)
        for name in written:
            f.write('    <file>icons/%s.svg</file>\n' % name)
        f.write("  </qresource>\n</RCC>\n")

    print("生成 %d 个图标 → %s" % (len(written), ICON_DIR))
    print("固定资源 %d 个：%s" % (len(FIXED_RESOURCES),
                                 ", ".join(FIXED_RESOURCES)))
    if stale:
        print("清理 %d 个已废弃图标：%s" % (len(stale), ", ".join(stale)))
    print("资源清单 → %s" % QRC_PATH)


if __name__ == "__main__":
    main()
