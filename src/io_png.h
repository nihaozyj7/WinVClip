// 图片读写与落盘：WIC 工厂、BGRA 缓冲、PNG 编码、防重名。
//
// 与缩略图解码（main.cpp）共用同一个 WIC 工厂实例和像素格式 GUID，
// 所以这两样必须放在头文件里对外暴露，而不是各自私有。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <string>
#include <vector>

namespace wv {

// BGRA，非预乘。缩略图与「另存为」共用这一种中间格式。
struct Rgba {
    int w = 0, h = 0;
    std::vector<BYTE> px;
};

// 进程内共享的 WIC 工厂（COM 单例，进程退出前 Release）
extern IWICImagingFactory* g_wic;

// 惰性创建 g_wic。任何用到 WIC 的路径都应先调它。
void InitWic();

// GUID_WICPixelFormat32bppBGRA / 32bppPBGRA 的本地定义：
// 不链接 uuid 库，避免为两个常量引入额外依赖。
extern const GUID kWicFmt32BGRA;
extern const GUID kWicFmt32PBGRA;

// 全量写入（分块，避免一次性大内存拷贝）
bool WriteAllBytes(const std::wstring& path, const void* data, size_t n);

// 把 BGRA 缓冲编码成 PNG 落盘
bool SaveRgbaAsPng(const Rgba& img, const std::wstring& path);

// 目标已存在时自动加 (1)(2)…，避免多文件复制时互相覆盖
std::wstring UniquePath(const std::wstring& p);

}  // namespace wv
