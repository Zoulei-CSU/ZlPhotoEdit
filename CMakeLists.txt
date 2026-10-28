# ============================================================================
#  CMakeLists.txt —— ZlPhotoEdit 的 CMake 构建配置
# ----------------------------------------------------------------------------
#  特点：同一份文件同时支持 Qt 5.15 与 Qt 6.x，无需任何修改。
#
#  核心技巧是下面两行 find_package：
#    find_package(QT NAMES Qt6 Qt5 ...)  先探测装的是哪个大版本
#    find_package(Qt${QT_VERSION_MAJOR} ...)  再用探测结果引用具体目标
#
#  只写 find_package(Qt5 ...) 就把工程锁死在 Qt5 了，这是最常见的错误。
#
#  构建方式（VS 2022 x64 Native Tools 命令提示符，cmake 与 ninja 自带于 VS）：
#    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#          -DCMAKE_PREFIX_PATH=C:\Qt\5.15.16-VS2022-rel
#    cmake --build build
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(ZlPhotoEdit VERSION 1.0.2 LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  Qt 探测 —— 优先 Qt6，找不到则 Qt5
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS
    Core Gui Widgets Svg Concurrent PrintSupport)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core Gui Widgets Svg Concurrent PrintSupport)

# Qt6 中 QT_VERSION_MAJOR 由 Qt6Config 自动提供；Qt5 的 find_package(QT NAMES ...)
# 也会设置它。但为了在"两者都没设"的极端情况下不出现空字符串拼接，
# 这里做一个兜底，避免后面 Qt${QT_VERSION_MAJOR}::Core 变成 Qt::Core 而报错。
if(NOT QT_VERSION_MAJOR)
    set(QT_VERSION_MAJOR 5)
endif()

message(STATUS "========================================")
message(STATUS " ZlPhotoEdit 构建配置")
message(STATUS " Qt 版本   : ${QT_VERSION}")
message(STATUS " 主版本号  : Qt${QT_VERSION_MAJOR}")
message(STATUS " 构建类型  : ${CMAKE_BUILD_TYPE}")
message(STATUS " 编译器    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "========================================")

# ---------------------------------------------------------------------------
#  C++ 标准
# ---------------------------------------------------------------------------
#  Qt6 强制要求 C++17，Qt5.15 也完整支持，所以统一写 17 两版都安全。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
#  Qt 元对象系统
# ---------------------------------------------------------------------------
#  这三项必须打开，对应 qmake 里自动完成的 moc / rcc / uic 处理。
#  漏掉 AUTOMOC 的典型症状是链接报 "undefined reference to vtable for ..."。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
#  源文件收集
# ---------------------------------------------------------------------------
#  CMake 这里用通配是安全的（不同于 qmake），因为 CONFIGURE_DEPENDS 会让
#  构建系统在新增文件时自动重新运行 cmake 配置步骤。
#  但仍要注意：通配只影响"是否被发现"，模块间的依赖方向仍靠代码纪律保证。
file(GLOB_RECURSE ZLPE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# 资源文件：resources.qrc（图标 + logo.png）。
# 用 EXISTS 判断避免文件缺失时配置阶段就报错。
set(ZLPE_RESOURCES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc")
    set(ZLPE_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc")
endif()

# Windows 资源文件：exe 文件本身的图标（不是窗口图标）。
#   .rc 不经过 Qt —— 它是 MSVC 的 rc.exe 编译的，产物直接进 PE 资源段。
#   两条构建路径指向同一个文件（qmake 侧见 ZlPhotoEdit.pro 的 RC_FILE）。
#   仅 WIN32：rc.exe 是 MSVC 工具链的一部分，别的平台会构建失败。
set(ZLPE_WIN_RESOURCES "")
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc")
    set(ZLPE_WIN_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc")
endif()

# ---------------------------------------------------------------------------
#  可执行目标
# ---------------------------------------------------------------------------
add_executable(ZlPhotoEdit
    ${ZLPE_SOURCES}
    ${ZLPE_RESOURCES}
    ${ZLPE_WIN_RESOURCES}
)

# .rc 由 MSVC 的 rc.exe 编译。CMake 对 .rc 有内置支持（enable_language(RC)
# 由 project(... CXX) 自动带上），这里只需要把包含目录指到 resources/ ——
# app.rc 里的 "logo.ico" 是相对路径，rc.exe 默认只看 .rc 所在目录，
# 显式给上包含目录后无论 CMake 从哪个工作目录调用都能找到。
if(ZLPE_WIN_RESOURCES)
    set_source_files_properties(${ZLPE_WIN_RESOURCES} PROPERTIES
        COMPILE_FLAGS "/I \"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
    )
    target_compile_definitions(ZlPhotoEdit PRIVATE ZLPE_HAS_EXE_ICON=1)
endif()

target_include_directories(ZlPhotoEdit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ---------------------------------------------------------------------------
#  链接 Qt —— 用 Qt${QT_VERSION_MAJOR} 前缀，Qt5/Qt6 通吃
# ---------------------------------------------------------------------------
target_link_libraries(ZlPhotoEdit PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Svg
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::PrintSupport
)

# ---------------------------------------------------------------------------
#  多语言：.ts → lrelease → .qm → 生成 i18n.qrc → AUTORCC 内嵌
# ---------------------------------------------------------------------------
#  ★ 硬约束（用户明确要求）：翻译文件必须内嵌进可执行文件 ★
#    不发布独立的 translations/ 文件夹 —— 运行时从 ":/i18n/..." 读。
#    代价是构建期多两步（lrelease + 生成 qrc），收益是"拷贝一个 exe
#    就能用"，且从结构上消除"忘了带翻译目录"这类部署事故。
#
#  链路：
#    translations/zlphotoedit_zh_CN.ts
#      → lrelease（构建期，逐个 .ts 跑）
#      → build/i18n/zlphotoedit_zh_CN.qm
#      → packaging/i18n/gen_i18n_qrc.cmake 生成 build/i18n/i18n.qrc
#         （内容 = 真实产出的 .qm 清单，新增语言只改 ZLPE_LANG_CODES）
#      → AUTORCC 把 .qm 编进 rcc 数据 → 运行时 ":/i18n/xxx.qm"
#
#  ★ qmake 侧的同一套逻辑见 ZlPhotoEdit.pro 的「多语言」段 ★
#    两边必须是同一批 .ts、同一个 prefix，否则换构建路径就等于换语言集。
#
#  ★ lrelease 找不到时只警告、不失败 ★
#    翻译是增强项：缺它界面回落到源码文本（见 core/UiLanguage.cpp），
#    不该让一个可选的 Linguist 组件把整个构建挡死。CI 的四个
#    Linux/macOS job 都自带 Qt 完整包，不会触发这条分支。
# ---------------------------------------------------------------------------
set(ZLPE_I18N_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
set(ZLPE_LANG_CODES zh_CN zh_TW fr es ja ru)
set(ZLPE_TRANSLATIONS "")
foreach(_lang IN LISTS ZLPE_LANG_CODES)
    list(APPEND ZLPE_TRANSLATIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/zlphotoedit_${_lang}.ts")
endforeach()
set(ZLPE_I18N_QRC "${ZLPE_I18N_DIR}/i18n.qrc")

# ★ 发布构建必须带上翻译：缺 lrelease 直接 configure 失败 ★
#   默认 OFF（本地开发没装 Linguist 组件也能编），CI 的每个发布 job
#   都显式传 -DZLPE_REQUIRE_I18N=ON —— 否则"少装一个可选包"会一路
#   静默到用户手里，表现成"切中文还是英文"，而构建日志 0 错 0 警。
#   （2026-10-19 的真实事故：deb/rpm 两个 job 只装 qt6-base-dev /
#     qt6-qtbase-devel，容器里根本没有 lrelease，configure 只打了一条
#     WARNING，装出来的程序里一条翻译都没有。）
option(ZLPE_REQUIRE_I18N "翻译内嵌失败时终止配置（发布构建必须为 ON）" OFF)

# lrelease 的安装位置随发行方式漂移，四个来源都要找：
#   · Qt 官方安装包 → bin/lrelease（与 qmake 同目录，走 IMPORTED 目标）
#   · Qt6 官方包    → libexec/lrelease（Qt6 起工具链从 bin 拆了出去）
#   · Debian/Ubuntu 的 Qt5 → /usr/bin/lrelease（find_program 默认路径）
#   · Debian/Ubuntu 的 Qt6 → /usr/lib/qt6/bin/lrelease（qt6-l10n-tools 包，
#     **不在 PATH 里**）；RPM 系 → /usr/lib64/qt6/bin/lrelease（qt6-linguist）
#
# ★ 用 qmake 的 IMPORTED 位置反推是最可靠的一路 ★
#   /usr/lib/qt6/bin/qmake 与 lrelease 同目录，而"从 Qt6Core_DIR 往上三级"
#   在 Debian 多架构布局下是错的：Qt6Core_DIR = /usr/lib/x86_64-linux-gnu/
#   cmake/Qt6Core，往上三级是 /usr/lib/x86_64-linux-gnu —— 既不是前缀，
#   底下也没有 bin/。这条启发式只对 Qt 官方安装包成立。
if(TARGET Qt${QT_VERSION_MAJOR}::qmake)
    get_target_property(_zlpe_qmake_exe
        Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    if(_zlpe_qmake_exe)
        get_filename_component(_zlpe_qt_bin "${_zlpe_qmake_exe}" DIRECTORY)
        find_program(QT_LRELEASE
            NAMES lrelease lrelease-qt5 lrelease-qt6
            HINTS "${_zlpe_qt_bin}" "${_zlpe_qt_bin}/../libexec")
    endif()
endif()
if(NOT QT_LRELEASE)
    # Qt6 的 cmake 目录形如 <prefix>/lib/cmake/Qt6Core，往上三级是 prefix
    foreach(_dir IN LISTS Qt${QT_VERSION_MAJOR}Core_DIR)
        if(_dir)
            find_program(QT_LRELEASE
                NAMES lrelease lrelease-qt5 lrelease-qt6
                HINTS "${_dir}/../../../bin" "${_dir}/../../../libexec")
        endif()
    endforeach()
endif()
if(NOT QT_LRELEASE)
    # 发行版把 Qt6 工具放在 lib/<multiarch>/qt6/bin 或 lib64/qt6/bin，
    # 这两个目录**不在 PATH 里**，必须显式给（否则装了 qt6-l10n-tools
    # 也找不到，症状是同一条静默回落）。
    find_program(QT_LRELEASE NAMES lrelease lrelease-qt5 lrelease-qt6
        PATHS
            /usr/lib/qt6/bin
            /usr/lib64/qt6/bin
            /usr/lib/x86_64-linux-gnu/qt6/bin
            /usr/lib/aarch64-linux-gnu/qt6/bin
        DOC "Qt Linguist 的 lrelease（用于把 .ts 编成内嵌 .qm）")
endif()
if(NOT QT_LRELEASE)
    find_program(QT_LRELEASE NAMES lrelease lrelease-qt5 lrelease-qt6)
endif()

# find_program 会把结果**缓存**：改过 PATH 再 configure 也还是旧值，
# 而 -DQT_LRELEASE=/不存在的路径 这种手误同样会被当成"找到了"。
# 缓存里的值必须真的可执行，否则等于没有。
if(QT_LRELEASE AND NOT EXISTS "${QT_LRELEASE}")
    message(WARNING "缓存的 QT_LRELEASE 不存在：${QT_LRELEASE} —— 当作没找到处理")
    unset(QT_LRELEASE CACHE)
    set(QT_LRELEASE "")
endif()

if(NOT QT_LRELEASE AND ZLPE_REQUIRE_I18N)
    message(FATAL_ERROR
        "找不到 lrelease，但本次构建要求内嵌翻译（ZLPE_REQUIRE_I18N=ON）。\n"
        "  翻译文件缺失时界面会静默回落到源码文本（英文），而构建本身 0 错 ——\n"
        "  这正是「切简体中文没反应」那一类事故的成因，所以在这里挡住。\n"
        "  装法：Debian/Ubuntu → apt install qt6-l10n-tools（或 qt6-tools-dev）；\n"
        "        Fedora/RHEL   → dnf install qt6-linguist；\n"
        "        Qt 官方安装包 → 装 Qt Linguist 组件。\n"
        "  仅本地开发可加 -DZLPE_REQUIRE_I18N=OFF 跳过。")
endif()

if(QT_LRELEASE)
    message(STATUS " lrelease     : ${QT_LRELEASE}")

    set(_zlpe_qm_outputs "")
    foreach(_ts IN LISTS ZLPE_TRANSLATIONS)
        get_filename_component(_base "${_ts}" NAME_WE)   # zlphotoedit_zh_CN
        set(_qm "${ZLPE_I18N_DIR}/${_base}.qm")
        # ★ 必须先建目录，且不能依赖生成器代劳 ★
        #   lrelease 自己一级目录也不建（-qm 指向缺失目录即
        #   "cannot create ...: No such file or directory"，exit 1，
        #   本机 Qt 5.15.16 实测，缺一级/缺两级同样报错）。
        #   而"生成器会不会建"随工具链漂移：
        #     · Ninja 会自动建输出目录 → 本机与 CI 的 Ninja job
        #       一直蒙混过关（2026-10-12 本机实测：输出放在缺失的
        #       gen/deep/ 下，Ninja 建目录后 lrelease 产出 26 字节 qm）；
        #     · Make 不建 → CI 的 AppImage-Qt6 job（rockylinux:8
        #       基础镜像只有 make，工作流刻意不装 ninja）在第一个
        #       自定义命令上直接失败。
        #   显式 make_directory 是与生成器无关的正确做法。
        add_custom_command(OUTPUT "${_qm}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZLPE_I18N_DIR}"
            COMMAND "${QT_LRELEASE}" "${_ts}" -qm "${_qm}"
            DEPENDS "${_ts}"
            COMMENT "lrelease ${_base}.ts -> ${_base}.qm"
            VERBATIM)
        list(APPEND _zlpe_qm_outputs "${_qm}")
    endforeach()

    # 期望产出的语言清单（逗号分隔）—— 供生成脚本逐个核对，
    # 少一个就构建期报错，而不是运行期静默回落。
    string(REPLACE ";" "," _zlpe_lang_codes "${ZLPE_LANG_CODES}")

    # qrc 的内容由"真实生成了哪些 .qm"决定（脚本里 glob），
    # 且 DEPENDS 全部 .qm —— lrelease 全部跑完后才生成 qrc。
    add_custom_command(OUTPUT "${ZLPE_I18N_QRC}"
        COMMAND "${CMAKE_COMMAND}"
                "-DQM_DIR=${ZLPE_I18N_DIR}"
                "-DOUT=${ZLPE_I18N_QRC}"
                "-DEXPECTED=${_zlpe_lang_codes}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/i18n/gen_i18n_qrc.cmake"
        DEPENDS ${_zlpe_qm_outputs}
        COMMENT "生成 i18n.qrc（内嵌翻译清单）"
        VERBATIM)

    # AUTORCC 会自动对 .qrc 跑 rcc；Makefile/Ninja 依据
    # OUTPUT 链保证 lrelease 先于 rcc 完成。
    target_sources(ZlPhotoEdit PRIVATE "${ZLPE_I18N_QRC}")
else()
    message(WARNING
        "找不到 lrelease —— 跳过翻译内嵌，界面语言将回落到源码文本。"
        "（不影响构建；装好 Qt Linguist 组件后重新 configure）")
endif()

# ---------------------------------------------------------------------------
#  MSVC 专用设置
# ---------------------------------------------------------------------------
if(MSVC)
    # ★ 关键：必须用 target_compile_options，不能用 add_compile_options ★
    #
    #  add_compile_options() 只对"它之后定义的 target"生效。本文件里
    #  add_executable() 在前面（第 84 行），所以 add_compile_options 加进来
    #  的 /utf-8 不会作用到 ZlPhotoEdit 上 —— 构建命令里根本看不到它。
    #
    #  target_compile_options() 显式绑定到 target，与声明顺序无关，永远可靠。
    #
    #  漏掉 /utf-8 的症状极具迷惑性：MSVC 按本地代码页（GBK）去解释 UTF-8
    #  字节流，于是某个中文字符的第 3 个字节可能恰好是 0x5C（反斜杠），
    #  把行尾的引号或换行"吃掉"，编译器随即报出一大串看似无关的错误 ——
    #  比如 "C2001: 常量中有换行符"、"C3646: 未知重写说明符"，
    #  甚至污染到 Qt 自己的头文件（qvariant.h / qimagereader.h）。
    #  这些报错位置全是"错误现场的邻居"，不是真正的病灶。
    target_compile_options(ZlPhotoEdit PRIVATE /utf-8)

    # 打开较严格的警告级别
    target_compile_options(ZlPhotoEdit PRIVATE /W3)

    # 打开弃用 API 警告 —— Qt6 迁移的关键提示手段
    target_compile_definitions(ZlPhotoEdit PRIVATE QT_DEPRECATED_WARNINGS)
endif()

# ---------------------------------------------------------------------------
#  平台相关
# ---------------------------------------------------------------------------
if(WIN32)
    # Windows 下作为 GUI 程序，不弹出额外的控制台窗口
    set_target_properties(ZlPhotoEdit PROPERTIES WIN32_EXECUTABLE TRUE)

    # 输出到 build/bin，与 .pro 的 DESTDIR 语义保持一致
    set_target_properties(ZlPhotoEdit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

if(APPLE)
    # ★ macOS 必须产出 .app bundle ★
    #   macdeployqt 只接受 .app（它要把 Qt framework 塞进 Contents/Frameworks/），
    #   裸可执行文件它直接报错退出 —— 所以 MACOSX_BUNDLE 不是可选项。
    #
    #   qmake 侧（.pro）默认就是 bundle，所以这里只是把 CMake 侧对齐。
    # ★ plist 里的语言清单由 ZLPE_LANG_CODES 推导，不再手抄一份 ★
    #   Apple 认 "zh-Hans" / "zh-Hant"，工程内部用 "zh_CN" / "zh_TW"，
    #   这里做一次映射；其余语言代码 Apple 与内部写法一致。
    #   末尾补 en —— 源码语言，也是 CFBundleDevelopmentRegion。
    set(ZLPE_MAC_LOCALES "en")
    foreach(_lang IN LISTS ZLPE_LANG_CODES)
        if(_lang STREQUAL "zh_CN")
            list(APPEND ZLPE_MAC_LOCALES "zh-Hans")
        elseif(_lang STREQUAL "zh_TW")
            list(APPEND ZLPE_MAC_LOCALES "zh-Hant")
        else()
            list(APPEND ZLPE_MAC_LOCALES "${_lang}")
        endif()
    endforeach()
    # plist 里是一个 <array>，需要 "<string>x</string>" 逐条拼成一段文本
    set(ZLPE_MAC_LOCALES_XML "")
    foreach(_loc IN LISTS ZLPE_MAC_LOCALES)
        string(APPEND ZLPE_MAC_LOCALES_XML "        <string>${_loc}</string>\n")
    endforeach()

    # 模板占位符是 @VAR@（与 qmake 侧共用一份），所以这里补上 qmake 的
    # 那套变量名，并用 configure_file(@ONLY) 生成到构建目录。
    set(EXECUTABLE_NAME "ZlPhotoEdit")
    set(PRODUCT_BUNDLE_IDENTIFIER "cn.zlstudio.ZlPhotoEdit")
    set(QMAKE_PKGINFO_TYPEINFO "????")
    set(ASSETCATALOG_COMPILER_APPICON_NAME "logo.icns")
    set(ZLPE_APP_VERSION "${PROJECT_VERSION}")
    set(MACOSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        @ONLY
    )

    set_target_properties(ZlPhotoEdit PROPERTIES
        MACOSX_BUNDLE TRUE
        # ★ 自定义 Info.plist 模板 ★
        #   默认模板里没有 CFBundleLocalizations，而系统只会把"应用声明
        #   支持的语言"里的哪一种传给进程 —— 不声明时，即便用户把简体
        #   中文排在首位，CFLocaleCopyCurrent() 也给 en_US，程序判成英文。
        #   这就是"自动跟随系统却跟到英文"的根因。
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "ZlPhotoEdit"
        # ★ CFBundleIdentifier 必须写成反向域名；macdeployqt / codesign 都读它。
        MACOSX_BUNDLE_GUI_IDENTIFIER "cn.zlstudio.ZlPhotoEdit"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "logo.icns"
    )
    # 图标：把 .icns 放进 Resources/ 并挂到 Resources 属性（CMake 会写进 Info.plist）。
    #   .icns 由 packaging/macos/ 下的脚本从 resources/logo.ico 生成（见 CI workflow）。
    #   文件不存在时跳过 —— 不能让"没有图标"阻止整个构建。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/logo.icns")
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/logo.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(ZlPhotoEdit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/logo.icns")
    endif()
endif()

# ---------------------------------------------------------------------------
#  部署脚本
# ---------------------------------------------------------------------------
#  ★ 这里**刻意不调用 qt_generate_deploy_app_script** ★
#
#  它的签名在 Qt6 内部变过三次，每个版本都能踩：
#    · Qt 6.5+：qt6_generate_deploy_app_script(TARGET t OUTPUT_SCRIPT v)
#    · Qt 6.4 ：旧签名还要 FILENAME_VARIABLE，少给就报
#                 "FILENAME_VARIABLE must be specified"
#    · Qt 6.0~6.2：又是另一套
#  而 Ubuntu 24.04 自带的正是 **6.4.2** —— CI 上两次撞墙都在这里。
#
#  更重要的是：**我们根本不需要它**。三条构建路径各有自己的部署方式：
#    · Windows  → `windeployqt`（workflow 里显式跑）
#    · Linux    → `linuxdeploy --plugin qt`（打进 AppImage）或依赖系统 Qt
#    · macOS    → `macdeployqt`（workflow 里显式跑）
#  所以直接不生成这个脚本，既少一个版本兼容坑，也不影响任何产物。
#
#  唯一要保留的是下面这段：让 target 知道怎么装到 CMAKE_INSTALL_PREFIX，
#  这样 `cmake --install` 与打包脚本能用同一套安装规则。
# ---------------------------------------------------------------------------
install(TARGETS ZlPhotoEdit
    RUNTIME DESTINATION bin          # Windows / Linux
    BUNDLE  DESTINATION .            # macOS（.app 放在前缀根目录）
)
