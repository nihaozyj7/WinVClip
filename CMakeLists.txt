cmake_minimum_required(VERSION 3.15)
project(winvclip CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件清单：每拆出一个模块，在这里加一行。
# 依赖方向单向：theme <- text_util <- diag/io <- glyph/emoji <- model <- layout <- paint <- panel
set(WINVCLIP_SOURCES
    src/main.cpp
    src/app_state.cpp
    src/text_util.cpp
    src/diag.cpp
    src/io_png.cpp
    src/glyph_data.cpp
)

add_executable(winvclip WIN32 ${WINVCLIP_SOURCES})

target_link_libraries(winvclip PRIVATE
    user32 gdi32 shell32 dwmapi d2d1 dwrite ole32 windowscodecs comdlg32 imm32 uiautomationcore)

target_compile_options(winvclip PRIVATE -Wall -Wextra -O2 -municode)
target_link_options(winvclip PRIVATE -municode)
