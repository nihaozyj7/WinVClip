cmake_minimum_required(VERSION 3.15)
# 需要 C：vendored 的 sqlite3.c（third_party）是纯 C 源文件
project(winvclip C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 目录结构与依赖方向（单向，下层不得反向 include 上层）：
#
#   core/       零依赖：主题常量、文本工具、文字度量、诊断日志、PNG 编解码、数据结构、全局状态
#   render/     绘制原语、字体、图标           -> core
#   image/      位图转换、缩略图               -> core, render
#   glyph/      字形库、emoji 图集             -> core, render
#   clipboard/  剪贴板采集、写回与注入         -> core, render, image
#   model/      剪贴板历史模型与视图重建       -> core
#   ui/         布局、绘制、面板输入、插入符探测 -> 以上全部
#   app/        入口、钩子、窗口过程           -> 全部
#
# include 一律从 src/ 起写全路径（如 "core/theme.h"），不依赖「包含文件所在目录」的隐式搜索。

set(WINVCLIP_SOURCES
    src/app/main.cpp
    src/app/hook.cpp
    src/app/startup.cpp
    src/app/tray.cpp
    src/core/app_state.cpp
    src/core/diag.cpp
    src/core/history_store.cpp
    src/core/io_png.cpp
    src/core/settings.cpp
    src/core/text_measure.cpp
    src/core/text_util.cpp
    src/core/util.cpp
    src/render/dc.cpp
    src/render/icons.cpp
    src/render/fonts.cpp
    src/image/raster.cpp
    src/image/thumb.cpp
    src/glyph/emoji_atlas.cpp
    src/clipboard/capture.cpp
    src/clipboard/paste.cpp
    src/model/clip_model.cpp
    src/ui/layout.cpp
    src/ui/paint.cpp
    src/ui/caret.cpp
    src/ui/actions.cpp
    src/ui/panel.cpp
    src/ui/settings_win.cpp
    src/ui/showhide.cpp
    src/glyph/glyph.cpp
    src/glyph/glyph_data.cpp
    # vendored 的 SQLite amalgamation（公共领域，单文件无外部依赖）。
    # 历史持久化用它：页级读写，正文/图片尺寸交给数据库自己处理，
    # 不像原先的 history.dat 那样每次改动都要全量重写。
    third_party/sqlite3.c
)

# 程序图标：源 icon 在 resources/app.ico（由 tools/make_icon.py 从 assets/app.png 生成），
# 经 configure_file 换成绝对路径后交给 windres。
#
# 资源不走「把 .rc 加进 add_executable」这条常规路：本机 binutils 2.47 的 ld 合并 .rsrc 时
# 会把每个 icon 叶误判成 duplicate leaf，链接直接失败。所以改成链接后由
# tools/attach_icon.py 把 windres 生成的资源节贴到 exe 上（脚本内有详细说明）。
set(WINVCLIP_RES_ICO ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico)
set(WINVCLIP_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
set(WINVCLIP_RES_OBJ ${CMAKE_CURRENT_BINARY_DIR}/app_icon.obj)
configure_file(resources/app.rc.in ${WINVCLIP_RC} @ONLY)

add_executable(winvclip WIN32 ${WINVCLIP_SOURCES})

# 图标和 .rc 都先拷进构建目录，windres 只吃相对文件名：它按本地代码页（GBK）解析 .rc 里的
# 路径，工程放在含中文的目录（如 Desktop\项目合集\...）时，绝对路径会变成乱码打不开，
# 报 "can't open icon file ... Illegal byte sequence"。工作目录固定成构建目录，
# 这样 cmd 的 cwd 是它、windres 看到的参数全是 ASCII。
add_custom_command(TARGET winvclip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WINVCLIP_RES_ICO} ${CMAKE_CURRENT_BINARY_DIR}/app.ico
    COMMAND windres -O coff -o app_icon.obj app.rc
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tools/attach_icon.py winvclip.exe app_icon.obj
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "贴上程序图标资源")

# 图标是链接后附加的，不是编译单元：不声明这层依赖的话，改 app.ico 后 ninja 认为
# 没有任何编译产物过期（"no work to do"），exe 里还是旧图标。
set_property(TARGET winvclip APPEND PROPERTY LINK_DEPENDS ${WINVCLIP_RES_ICO})

# 第三方源码不跟着吃本项目的 -Wall -Wextra：sqlite3.c 会喷出几千条警告，
# 把真正需要看的警告淹掉。其余三个宏是本项目需要的裁剪：
#   THREADSAFE=0          所有数据库访问都在 UI 主线程（没有后台写线程），不需要互斥；
#   OMIT_LOAD_EXTENSION   不加载任何扩展，去掉一条不必要的攻击面；
#   DQS=0                 禁止用双引号当字符串字面量，避免 SQL 里悄悄写错的引号被放过。
set_source_files_properties(third_party/sqlite3.c PROPERTIES
    COMPILE_OPTIONS "-w"
    COMPILE_DEFINITIONS "SQLITE_THREADSAFE=0;SQLITE_OMIT_LOAD_EXTENSION;SQLITE_DQS=0")

target_include_directories(winvclip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_link_libraries(winvclip PRIVATE
    user32 gdi32 shell32 dwmapi d2d1 dwrite ole32 oleaut32 windowscodecs comdlg32 imm32 uiautomationcore)

target_compile_options(winvclip PRIVATE -Wall -Wextra -O2 -municode)
target_link_options(winvclip PRIVATE -municode)
