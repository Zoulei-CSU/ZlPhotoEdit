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

project(ZlPhotoEdit VERSION 0.1 LANGUAGES CXX)

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

# 资源文件：resources.qrc（图标 + logo.svg）。
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
    set_target_properties(ZlPhotoEdit PROPERTIES
        MACOSX_BUNDLE TRUE
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
#  部署（可选）
# ---------------------------------------------------------------------------
#  Qt6 提供了官方的部署脚本生成器，Qt5 没有（Qt5 需手动跑 windeployqt）。
#
#  ★ OUTPUT_SCRIPT 参数是 **Qt 6.5 才加的** ★
#    在 6.4（Ubuntu 24.04 自带的版本）上调用会直接报：
#      CMake Error at Qt6CoreMacros.cmake: Unexpected arguments: OUTPUT_SCRIPT;...
#    这是 CI 上真实撞到的 —— 本地只有 Qt5，这条分支从没被编译过。
#    所以这里按 6.5 分界：>= 6.5 用带 OUTPUT_SCRIPT 的新签名，6.0~6.4 用旧签名。
#
#    旧签名（Qt 6.0~6.4）会把部署脚本作为安装规则注册，不需要我们自己 install()。
if(QT_VERSION_MAJOR EQUAL 6)
    if(QT_VERSION VERSION_GREATER_EQUAL 6.5)
        qt_generate_deploy_app_script(
            TARGET ZlPhotoEdit
            OUTPUT_SCRIPT deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
        install(SCRIPT ${deploy_script})
    else()
        # Qt 6.0 ~ 6.4：旧签名（无 OUTPUT_SCRIPT）
        qt_generate_deploy_app_script(
            TARGET ZlPhotoEdit
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
    endif()
endif()
