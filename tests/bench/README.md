# tests/bench —— 图标上色的开销对照

回答一个具体问题：**"渲染时上色"比"直接渲染"贵多少？** 尤其是浅色主题
（绝大多数用户的日常状态）下的稳态路径。

不属于常规自检（`run_tests.sh` 里没有它）—— 只在改动 `IconLibrary`
的上色/缓存实现后跑一次，确认没有把某个场景拖慢。

## 为什么新旧两版要编进同一个程序

跨进程比较会被编译差异、机器负载、CPU 降频干扰到 ±20%，
足以淹没 1.15× 这种量级的差异。这里把旧实现整体冻结进
`zl::icons_old` 命名空间，两个版本在同一进程里交替跑同一组场景，
差异只来自代码本身。

`IconLibrary_old.*` 取自引入上色之前的版本，**不要顺手改它** ——
它是基线。要改就另存一份新的。

## 编译

下面的命令**在仓库根目录**执行。先拿到一份含全部图标资源的 `qrc.cpp`：
优先复用当前构建目录里 `rcc` 已生成的那份（`find build* -name 'qrc_resources.cpp'`），
否则自己生成 —— 注意 **Qt6 的 `rcc` 在 `libexec/`，Qt5 才在 `bin/`**：

```sh
QT=/path/to/Qt/6.8.3/gcc_64
RCC_OUT=/tmp/bench_qrc.cpp
"$QT/libexec/rcc" resources/resources.qrc -o "$RCC_OUT"   # Qt5: "$QT/bin/rcc"

c++ -O2 -std=c++17 -fPIC \
    -DQT_NO_DEBUG -DQT_SVG_LIB -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB \
    -I src -I tests/bench \
    -isystem $QT/include \
    -isystem $QT/include/QtCore -isystem $QT/include/QtGui \
    -isystem $QT/include/QtWidgets -isystem $QT/include/QtSvg \
    tests/bench/bench_icons.cpp tests/bench/IconLibrary_old.cpp \
    src/ui/IconLibrary.cpp "$RCC_OUT" \
    -L$QT/lib -lQt6Svg -lQt6Widgets -lQt6Gui -lQt6Core -o /tmp/bench

QT_QPA_PLATFORM=offscreen /tmp/bench
```

Windows 下把 `c++` 换成 `cl.exe` 并改成 `/I` `/link` 形式即可
（`run_tests.sh` 里有现成的写法可抄）。

`bench_parts.cpp` 是另一支程序，单独量"渲染一张 / 上色一张 / 拷一张"的单价，
不需要旧实现 —— 把上面命令里的 `bench_icons.cpp tests/bench/IconLibrary_old.cpp src/ui/IconLibrary.cpp`
整体换成 `tests/bench/bench_parts.cpp` 即可（`qrc.cpp` 仍然要带，它引用了 `:/icons/tool_brush.svg`）。

## 怎么读结果

* **只看同一行的两个数**，不要跨行比较绝对值（场景不同）。
* `x1.15` 这类倍率必须**连跑三次都稳定**才算数；抖动超过 ±0.1 就重跑，
  别拿单次结果下结论。
* 关键的一行是 `③b 每帧路径`。它进的是"重绘时每帧都要走"的路径，
  绝对耗时比倍率更重要 —— 现在是**比改造前快 4~5 倍**。
* 冷构建、换主题都是一次性开销，各多十几毫秒属于可接受范围
  （详见 `docs/实现铁律.md` §10.8.2）。

## 已知的坑

* 用 `clock()`（CPU 时间）计时，不要换成 `QElapsedTimer`：
  后者是墙钟，offscreen 下会被别的进程抢占带跑。
* 新旧头文件原本 include guard 同名，不改会导致新版头被整体跳过。
  已改成 `ZLPE_UI_ICONLIBRARY_OLD_H`，别再改回去。
