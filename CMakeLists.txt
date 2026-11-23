cmake_minimum_required(VERSION 3.15)
project(winvclip CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(winvclip WIN32 src/main.cpp src/glyph_data.cpp)
target_link_libraries(winvclip PRIVATE user32 gdi32 shell32 dwmapi d2d1 dwrite ole32 windowscodecs comdlg32 imm32)
target_compile_options(winvclip PRIVATE -Wall -Wextra -O2 -municode)
target_link_options(winvclip PRIVATE -municode)
