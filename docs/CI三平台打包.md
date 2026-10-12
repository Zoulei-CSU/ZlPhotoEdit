# CI 三平台打包 —— 方式与经验

> 状态：**已实施并发布**（v0.1.0，2026-10-12，14 轮 CI 迭代后 8/8 全绿）。
> 本文回答"**怎么做**"和"**坑在哪**"，作为以后类似 Qt 应用打包的指导文件。
> 工作流：`.github/workflows/release.yml`；打包脚本：`packaging/linux/`、`packaging/macos/`；
> glibc 断言脚本：`packaging/linux/check_appimage_glibc.py`。
> 逐轮调试的原始日志结论在 `.workbuddy/memory/2026-10-11-CI实施.md`（本地记忆，不入库）。

---

## 〇、成果：8 个 job / 7 个产物

| # | 产物 | 构建环境 | Qt | glibc / 系统要求 | 大小 |
|---|---|---|---|---|---|
| 1 | `ZlPhotoEdit-<ver>-win64.zip` | windows-2022 + windeployqt | 6.8.1 | Win10+ | 11.9 MB |
| 2 | `ZlPhotoEdit-<ver>-x86_64.AppImage` | **ubuntu:18.04 容器** | 5.15.2 | **glibc 2.17+** | 24.4 MB |
| 3 | `ZlPhotoEdit-<ver>-glibc228-x86_64.AppImage` | **rockylinux:8 容器** | 6.8.1 | glibc 2.28+ | 33.6 MB |
| 4 | `zlphotoedit_<ver>_amd64.deb` | ubuntu:24.04（系统 Qt6） | 系统自带 | Ubuntu 24.04+ | 0.5 MB |
| 5 | `zlphotoedit-<ver>-1.fc43.x86_64.rpm` | fedora:43（系统 Qt6） | 系统自带 | Fedora 43 | 0.5 MB |
| 6 | `ZlPhotoEdit-<ver>-macos-x86_64.zip` | macos-15-intel + macdeployqt | 5.15.2 | macOS 10.13+ | 10.3 MB |
| 7 | `ZlPhotoEdit-<ver>-macos-arm64.zip` | macos-14 + macdeployqt | 6.8.1 | macOS 11.0+ | 20.2 MB |

★ deb/rpm **只装程序本身**（主程序 + 图标 + desktop/spec 元数据），**依赖系统 Qt** —— 包只有 0.5 MB。
★ 两个 AppImage 是**自包含**的（Qt 全打包在里面），代价是体积。
★ 未签名（macOS 无 Developer 账号）：用户需右键→打开，README 已注明。

---

## 一、三条设计原则（决定方案长什么样）

### 原则 1：在最老的、还要支持的系统上构建

AppImage 官方最佳实践的原文要求：

> *"The ingredients used in your AppImage should not be built on a more recent base system
> than the oldest base system your AppImage is intended to run on."*

**产物 glibc 下限 = 构建环境 glibc 下限 ∪ 所有被打包进来的库的 glibc 下限**。后者是隐形的——
Qt 库、ICU、系统库全都会被 linuxdeploy 收进来，任何一个来自新发行版，整个包的下限就被顶上去。
**唯一可靠的度量是解包后逐 ELF 扫 `GLIBC_2.x` 符号**（见原则 3）。

GitHub-hosted runner 里**已经没有 glibc < 2.31 的镜像**（ubuntu-20.04 于 2025-04-15 下线），
所以要低 glibc 必须**在 job 里跑老发行版容器**（`container: ubuntu:18.04`）。

### 原则 2：Qt 版本跟着 glibc 目标走——基线写在归档名里

Qt 官方 Linux 二进制的**构建基线直接写在归档文件名里**，这是选版本的第一判据：

| Qt 版本 | 归档名样例 | 构建基线 | glibc 下限 | 覆盖 |
|---|---|---|---|---|
| 5.15.2 | `qtbase-Linux-RHEL_7_6-GCC-...` | RHEL 7.6 | **2.17** | CentOS/RHEL 7+、Debian 9+ |
| 6.8.1 | `qtbase-Linux-RHEL_8_10-GCC-...` | RHEL 8.10 | **2.28** | Ubuntu 20.04+ / Debian 11+ |
| 6.10+ | Ubuntu 24.04 构建 | 2.39 | 2.34+ | 更窄，不推荐用于低 glibc |

⇒ **想覆盖老系统就用 Qt5.15.2（glibc 2.17）；Qt6 只能到 2.28**。两者都出（本项目的方案）最稳。
★ Qt 6.8 官方二进制要 **GCC ≥ 11** 才能链接使用 → 容器用 `rockylinux:8` + `gcc-toolset-11`
（el8 的开发工具集，`source /opt/rh/gcc-toolset-11/enable`）。
★ Qt 只保证**同大版本内**向后二进制兼容 → deb/rpm 依赖系统 Qt 时，**必须按目标发行版自带的 Qt 版本编**
（故 deb/rpm 走发行版矩阵，而不是拿一份二进制到处塞）。

### 原则 3："编出来" ≠ "低 glibc 打出来了"——必须实测，并做成硬断言

AppImage 文件本身**看不出** glibc 要求。本项目第 9 轮 CI 全绿时实测 Qt5 AppImage 竟是 **2.27**
（元凶只有两个文件：`libsystemd.so.0`→2.27、`libbsd.so.0`→2.25）。

**做法**：构建期解包 AppImage，逐 ELF 扫 `GLIBC_2.x` 字符串（纯静态、不执行），超过上限即构建失败：

```bash
./ZlPhotoEdit-*.AppImage --appimage-extract        # 容器无 FUSE，需先 export APPIMAGE_EXTRACT_AND_RUN=1
python3 packaging/linux/check_appimage_glibc.py squashfs-root 17   # 17 = GLIBC_2.17
rm -rf squashfs-root
```

实测输出（第 13 轮）：`扫描到 53 个 ELF；最高需求 = GLIBC_2.17；上限 = GLIBC_2.17 → OK`。
**没有这个断言，"低 glibc"会静默退化**——某次依赖变更把新库拖进来，没人会发现。

---

## 二、最终架构：8 个 job

```
release.yml
├─ windows            windows-2022      Qt6.8.1 宿主安装 → windeployqt → zip
├─ appimage-qt5       ubuntu-latest     Qt5.15.2 宿主下载 → 挂载进 ubuntu:18.04 容器 → linuxdeploy → AppImage(2.17)
├─ appimage-qt6       ubuntu-latest     Qt6.8.1  宿主下载 → 挂载进 rockylinux:8 容器 → linuxdeploy → AppImage(2.28)
├─ linux-packages     matrix: ubuntu:24.04 / fedora:43   系统 Qt6 → make-deb.sh / make-rpm.sh
├─ macos-x86_64       macos-15-intel    Qt5.15.2 → macdeployqt → zip
├─ macos-arm64        macos-14          Qt6.8.1  → macdeployqt → zip
└─ release            汇总下载 7 个产物 → softprops/action-gh-release 发布
```

- `fail-fast: false`，7 个构建 job 互不阻塞；`concurrency` 同分支重复触发自动取消（省时间）。
- **统一用 CMake 构建**（qmake 仅本地开发用）：三条路同一条命令，
  `cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt路径>`。
- Qt 的安装方式：Windows/macOS 用 `jurplel/install-qt-action`（宿主直接装）；
  Linux 两个 AppImage job **在宿主下载 Qt、再挂载进容器**（原因见坑 2）。
- AppDir 内容 = 主程序 + `.desktop` + 256×256 **PNG** 图标（AppImage 要 PNG，SVG 不行）+ Qt 全家桶。
- macOS 图标：`packaging/macos/make-icns.sh` 从 PNG 生成 `.icns` 再进 `.app`。

---

## 三、真正的坑（14 轮 CI 换来的，按代价排序）

### 坑 1：容器里跑不了任何 GitHub action —— checkout 必须在宿主做

`actions/checkout@v3/v4` 被 GitHub **强制跑在 Node 24 上，Node 24 要 glibc ≥ 2.28**。
⇒ `ubuntu:18.04`（glibc 2.27）容器里**任何 action 都起不来**（不是 checkout 的错，换 v3 也没用）。

**解法**：job 不加 `container:`；在宿主 runner 上 checkout + 下载 Qt，
再用 `docker run -v $PWD:/ws -w /ws ubuntu:18.04 bash -c '...'` 把整个工作区挂进容器执行构建/打包。
宿主与容器之间只靠挂载传递，不靠 action。

### 坑 2：老发行版容器里装不了 aqtinstall —— Qt 在宿主下、挂载进容器

- bionic 自带 Python 3.6：新版 aqtinstall 编 `psutil` 直接 `UnicodeDecodeError`；
  锁旧版又卡 `inflate64` 等纯 Python 3.7+ 的依赖。
- 不要试图在容器里 `pip install aqtinstall`。

**解法**：宿主 `jurplel/install-qt-action`（或 pip 装 aqtinstall）下好 Qt → `docker run -v <Qt路径>:/opt/Qt` 挂载。

### 坑 3：linuxdeploy 排除库——CLI 参数只管自己，环境变量才穿透插件子进程

只想排除"本该由系统提供"的两个库时：

- `./linuxdeploy --exclude-library libsystemd.so.0` **只作用于 linuxdeploy 自己的 AppDir 状态**；
- **qt 插件是独立子进程**，只认自己收到的排除列表 → 库照样被打包（第 10 轮实测：加了 CLI 参数仍是 2.27）。

**解法：两条路都写**（命令行 + 环境变量，双保险）：

```bash
export LINUXDEPLOY_EXCLUDED_LIBRARIES="libsystemd.so.0;libbsd.so.0"   # 分号分隔，子进程继承
./linuxdeploy --appdir AppDir --plugin qt --output appimage \
  --exclude-library "libsystemd.so.0" --exclude-library "libbsd.so.0"
```

日志里确认生效的标志：`Skipping deployment of blacklisted library ... libsystemd.so.0`（来自 `[qt/stdout]`）。

**为什么要排除这两个**（依赖链实测）：
`libqxcb.so → libQt5DBus → libdbus-1 → libsystemd.so.0`(需 2.27)、`libXdmcp.so.6 → libbsd.so.0`(需 2.25)。
这两个库在所有现代发行版都由**系统**提供，从 18.04 容器拖出去只会把下限抬到 2.27，与目标相悖。

### 坑 4：`docker run bash -c '...'` 内层脚本里绝不许出现单引号

连**注释里的** `';'` 都会把外层的 `bash -c '` 提前闭合 → 整段脚本被当成一个超长文件名 →
报 `: File name too long`、**退出码 126，而错误信息完全看不出真实原因**（第 11 轮卡了整整一轮）。

**解法**：写 docker 内层脚本前后，数一遍外层单引号数必须 = 2；注释里也别用单引号（改中文表述）。

### 坑 5：容器是 POSIX locale —— Python 的 stdout 是 ASCII

老容器没有 UTF-8 locale，Python 3.6 的 stdout 编码退化为 ASCII → **中文 print 直接
`UnicodeEncodeError`**。第 12 轮就是这个：AppImage 构建成功、排除生效，自证脚本反而崩在 print 上，
变成一个"假失败"。

**解法**（双保险）：脚本开头做编码兜底（3.7+ `sys.stdout.reconfigure(errors='replace')`；
3.6 用 `codecs.getwriter` 包一层），命令行前再加 `PYTHONIOENCODING=utf-8`。

### 坑 6：deb 产物会静默消失——OUTDIR 必须在 `cd` 之前绝对化

打包脚本常见套路：`STAGE=$(mktemp -d)` … `trap "rm -rf $STAGE" EXIT` … `cd "$STAGE"` 组装目录结构。
如果在 `cd` **之后**才用相对路径写产物 → 产物落进临时目录、被 trap 删掉，
**日志还打印着"已生成"**，直到 upload 阶段才报 `No files were found`。

**解法**：`OUTDIR="$(cd "$OUTDIR" && pwd)"` 放在任何 `cd` 之前；workflow 里再加一步"核对产物"
（文件不在立刻 fail，别等 upload）。

### 坑 7：各平台装 Qt 依赖的"一次性补齐"清单（别一轮轮挤牙膏）

linuxdeploy 的 qt 插件按 soname 逐个收集，**缺一个就整步失败，而且一次只报一个** → 会把人一轮轮遛。

- **bionic（18.04）**：`archive.ubuntu.com` 仍在服务（200），`old-releases` 是 404，别换；
  bionic 自带 cmake 3.10.2 < 项目要求的 3.16 → 加 **Kitware apt 源**。
- **rockylinux:8**：先 `dnf install epel-release` 并**启用 `powertools`**（el8 叫 powertools，el9 才叫 CRB），
  否则 `xcb-util-*` 一律 `No match`。已知要补的：
  `xcb-util-cursor`（**只在 EPEL 8**，Rocky 自带仓库全都没有）、`cups-libs`（Qt PrintSupport → libcups.so.2；
  BaseOS 里，配套的 `cups-devel` 在 AppStream）、`libSM`/`libICE`/`libX11-xcb`/`glib2`。

### 坑 8：Qt ≥ 6.7 的 Linux 仓库包名从 `gcc_64` 改成了 `linux_gcc_64`

传给 aqtinstall 的 arch 写错会报 `The packages ['qt_base'] were not found while parsing XML`。
**判据**：直接读仓库的 `Updates.xml`，看 `<DownloadableArchives>` 里的真实归档名，别凭记忆。

### 坑 9：git 推送不走 all_proxy

- **curl 只认 `all_proxy`**（设 `https_proxy` 反而会 sideband disconnect）；
- **git 只认 `http.proxy`/`https.proxy`**（`all_proxy` 对 git 无效），大包推送再加 `-c http.version=HTTP/1.1`：

```bash
git -c http.proxy=socks5h://127.0.0.1:7897 -c https.proxy=socks5h://127.0.0.1:7897 \
    -c http.version=HTTP/1.1 push origin main
```

### 坑 10：CI 日志怎么看、tag 怎么发

- 远端 URL 内嵌 token，可直接调 API 拿**真实日志**（排查必须基于日志，不许猜）：

```bash
TOK=$(git remote get-url origin | sed -n 's#.*://[^:]*:\([^@]*\)@.*#\1#p')
export all_proxy=socks5h://127.0.0.1:7897 https_proxy= http_proxy=
curl -sL -H "Authorization: Bearer $TOK" \
  "https://api.github.com/repos/<o>/<r>/actions/jobs/<JOB_ID>/logs"
```

- **调试期**：`workflow_dispatch` 触发 main（干净、不动 tag）；**发布期**：打 `v*.*.*` tag。
  release job 的条件是 `startsWith(github.ref, 'refs/tags/v')` —— dispatch 到 main 时它**不会**跑，
  这是特性不是 bug。
- tag 指错提交时：`git tag -f v0.1.0 <sha>` + 强推即可重切（确认远端没有已发布的 Release 再动）。

---

## 四、复用清单（下一个 Qt 应用照着抄）

1. **先定 glibc 目标** → 反推 Qt 版本（查归档名里的 RHEL 基线）→ 反推构建容器。
2. **workflow 结构**：7 个构建 job `fail-fast: false` + 1 个 release job；`concurrency` 取消同分支重复触发。
3. **低 glibc job 模板**：宿主 checkout + 宿主装 Qt → `docker run -v` 挂载 → 容器内 cmake 构建 + linuxdeploy。
4. **打包三件套**：`.desktop` + 256×256 PNG 图标 + `linuxdeploy --plugin qt`。
5. **排除库双写**：`LINUXDEPLOY_EXCLUDED_LIBRARIES`（分号）+ `--exclude-library`。
6. **glibc 断言**：解包 + 逐 ELF 扫 `GLIBC_2.x`，超上限即 fail（抄 `packaging/linux/check_appimage_glibc.py`）。
7. **依赖系统 Qt 的包（deb/rpm）**：按目标发行版矩阵构建；产物路径先绝对化再 `cd`。
8. **docker 内层脚本**：单引号数 = 2；注释里避开单引号。
9. **中文输出**：脚本编码兜底 + `PYTHONIOENCODING=utf-8`。
10. **推送/取日志**：git 用 `-c http.proxy`；curl 用 `all_proxy`；远端 token 直接读 API 日志。
11. **macOS**：`macos-15-intel` 出 x86_64、`macos-14` 出 arm64；无签名就在 README 写"右键→打开"。

---

## 附：引用来源

- [AppImage Best Practices —— 在最老支持系统上构建](https://docs.appimage.org/reference/best-practices.html)
- [Qt 5.15 Tools and Versions —— 官方二进制构建基线 RHEL 7.6](https://wiki.qt.io/Qt_5.15_Tools_and_Versions)
- [Qt for Linux —— 官方安装器构建基线；6.8+ 需 glibc 2.28、6.10+ 需 2.34](https://doc-snapshots.qt.io/qt6-dev//linux.html)
- [linuxdeploy —— AppDir 打包与 exclude 机制](https://github.com/linuxdeploy/linuxdeploy)
- [actions/runner-images —— 可用镜像清单](https://github.com/actions/runner-images)
- [ubuntu-20.04 runner 下线（2025-04-15）](https://github.com/actions/runner-images/issues/11101)
- [macOS 13 runner 下线（2025-12-04）](https://github.blog/changelog/2025-09-19-github-actions-macos-13-runner-image-is-closing-down/)
- [GitHub hosted runners 参考（含 macos-15-intel）](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [Qt macOS 部署目标 10.13](https://doc.qt.io/qt-5/macos.html)
