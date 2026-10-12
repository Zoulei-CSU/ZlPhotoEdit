# ============================================================================
#  gen_i18n_qrc.cmake —— 生成内嵌翻译资源的 qrc 文件
# ----------------------------------------------------------------------------
#  用法（由 CMakeLists.txt「多语言」段的 add_custom_command 调用）：
#    cmake -DQM_DIR=<.qm 所在目录> -DOUT=<输出的 .qrc 路径> -P gen_i18n_qrc.cmake
#
#  ★ 为什么不在 CMakeLists.txt 里直接 file(WRITE ...) ★
#    qrc 的内容必须等于"这一步**真实生成了**哪些 .qm"。lrelease 是
#    构建期跑的，configure 期 glob 会漏掉"这次构建新加的"语言；
#    而把这个 -P 脚本挂在所有 .qm 的 add_custom_command 之后，
#    qrc 就永远与真实的产物集合一致 —— 新增语言只改
#    ZLPE_TRANSLATIONS 一处，qrc 自动跟上。
#
#  ★ 生成的 qrc 与 .qm 必须同目录 ★
#    qrc 里 <file> 是相对路径（相对 qrc 文件自身所在目录）。
#    两者不同目录时这里直接 FATAL_ERROR，而不是生成一个
#    "configure 通过、运行时翻译消失"的隐性炸弹。
# ============================================================================

if(NOT DEFINED QM_DIR OR NOT DEFINED OUT)
    message(FATAL_ERROR
        "gen_i18n_qrc.cmake 需要 -DQM_DIR=<目录> -DOUT=<文件>")
endif()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
if(NOT _out_dir STREQUAL QM_DIR)
    message(FATAL_ERROR
        "OUT (${OUT}) 必须与 QM_DIR (${QM_DIR}) 同目录："
        "qrc 的 <file> 是相对 qrc 自身目录的路径")
endif()

file(GLOB _qm_files RELATIVE "${QM_DIR}" "${QM_DIR}/*.qm")
list(SORT _qm_files)

# prefix "/i18n" 与 ZlPhotoEdit.pro 的 qmake 侧保持一致 ——
# 两条构建路径必须产出同一个运行时路径（:/i18n/...），
# 否则换构建路径就等于换语言集，而那种差异没有任何编译期信号。
set(_content "<!DOCTYPE RCC><RCC version=\"1.0\">\n")
string(APPEND _content "<qresource prefix=\"/i18n\">\n")
foreach(_f IN LISTS _qm_files)
    string(APPEND _content "  <file>${_f}</file>\n")
endforeach()
string(APPEND _content "</qresource>\n</RCC>\n")

file(WRITE "${OUT}" "${_content}")
message(STATUS "生成 ${OUT}：${_qm_files}")
