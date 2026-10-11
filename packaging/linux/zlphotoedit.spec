# ============================================================================
#  zlphotoedit.spec —— ZlPhotoEdit 的 RPM 打包脚本
# ----------------------------------------------------------------------------
#  用法： rpmbuild -bb --define "_ver <版本>" --define "_srcdir <源码根>" \
#                    --define "_stage <预编译产物目录>" packaging/linux/zlphotoedit.spec
#
#  ★ 为什么用 %install 直接拷贝预编译产物 ★
#
#    CI 里二进制**已经在容器内编好了**（用系统的 qt6-qtbase-devel）。
#    再写一遍 %build 让 rpmbuild 重编一遍纯属浪费，而且会在 Fedora 容器里
#    引入 cmake/ninja 的全部构建依赖。这里走的是 "binary-only rpm" 的路子：
#    %prep/%build 空着，%install 只做"摆放"。
#
#  ★ 依赖为什么不手写 ★
#
#    现代 rpmbuild 会**自动**为 ELF 二进制生成 Requires ——
#    它扫出 libQt6Core.so.6(Qt_6.10)(64bit) 这种**带符号版本**的依赖，
#    比手写 "qt6-qtbase" 精确得多。所以下面只写一个宽松的兜底 Requires。
# ============================================================================

Name:           zlphotoedit
Version:        %{_ver}
Release:        1%{?dist}
Summary:        逐鹿 图片编辑器 —— 轻量级图片编辑器

License:        MIT
URL:            https://github.com/Zoulei-CSU/ZlPhotoEdit
Source0:        %{name}-%{_ver}.tar.gz

# ★ 不打包任何 Qt 库 ★ —— 依赖系统自带的 Qt6（与 deb 侧策略一致）。
#   下面这条是"最低要求"，精确的 soname 依赖由 rpmbuild 自动补。
Requires:       qt6-qtbase
Requires:       qt6-qtsvg

%description
基于 Qt 的轻量级图片编辑器：多图层与 22 种混合模式、矩形/椭圆/羽化选区、
魔术棒、15 个工具、曲线/色阶/亮度对比度、旋转翻转裁剪缩放、多标签页、
标尺/参考线/网格、另存为 ICO、屏幕截图。

纯 Qt 实现，不依赖任何第三方图像库；主程序不到 2 MB（不含 Qt 库）。

%prep
# 不需要解压源码 —— 二进制已在 CI 里构建好

%build
# 同上：跳过编译

%install
rm -rf %{buildroot}
# %{_stage} 由 CI 传入，指向已构建的产物 + 打包资源
install -Dm755 %{_stage}/ZlPhotoEdit                    %{buildroot}%{_bindir}/ZlPhotoEdit
install -Dm644 %{_stage}/zlphotoedit.desktop            %{buildroot}%{_datadir}/applications/zlphotoedit.desktop
install -Dm644 %{_stage}/zlphotoedit.png                %{buildroot}%{_datadir}/icons/hicolor/256x256/apps/zlphotoedit.png
install -Dm644 %{_stage}/README.md                      %{buildroot}%{_docdir}/%{name}/README.md

%files
%{_bindir}/ZlPhotoEdit
%{_datadir}/applications/zlphotoedit.desktop
%{_datadir}/icons/hicolor/256x256/apps/zlphotoedit.png
%{_docdir}/%{name}/README.md

%changelog
* Sun Oct 11 2026 Zoulei-CSU <zoulei.csu@gmail.com> - %{_ver}-1
- 自动构建的二进制包