cmake_minimum_required(VERSION 3.15)
project(winvclip CXX)

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
    src/core/app_state.cpp
    src/core/diag.cpp
    src/core/io_png.cpp
    src/core/text_measure.cpp
    src/core/text_util.cpp
    src/glyph/glyph.cpp
    src/glyph/glyph_data.cpp
)

add_executable(winvclip WIN32 ${WINVCLIP_SOURCES})

target_include_directories(winvclip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(winvclip PRIVATE
    user32 gdi32 shell32 dwmapi d2d1 dwrite ole32 windowscodecs comdlg32 imm32 uiautomationcore)

target_compile_options(winvclip PRIVATE -Wall -Wextra -O2 -municode)
target_link_options(winvclip PRIVATE -municode)
