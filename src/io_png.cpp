#include "io_png.h"

namespace wv {

IWICImagingFactory* g_wic = nullptr;

namespace {
const GUID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615,
                               {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kIidWicFactory   = {0xec5ec8a9, 0xc395, 0x4314,
                               {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
}  // namespace

const GUID kWicFmt32BGRA = {0x6fddc324, 0x4e03, 0x4bfe,
                            {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
const GUID kWicFmt32PBGRA = {0x6fddc324, 0x4e03, 0x4bfe,
                             {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

void InitWic() {
    if (g_wic) return;
    CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER,
                     kIidWicFactory, (void**)&g_wic);
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    const BYTE* p = (const BYTE*)data;
    while (ok && n > 0) {
        DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n;
        DWORD wr = 0;
        ok = WriteFile(h, p, chunk, &wr, nullptr) && wr == chunk;
        p += chunk;
        n -= chunk;
    }
    CloseHandle(h);
    return ok;
}

bool SaveRgbaAsPng(const Rgba& img, const std::wstring& path) {
    InitWic();
    if (!g_wic || img.px.empty() || img.w <= 0 || img.h <= 0) return false;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* opt = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &opt)) &&
        SUCCEEDED(fr->Initialize(opt))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(fr->SetSize((UINT)img.w, (UINT)img.h)) &&
            SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
            IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(fr->WritePixels((UINT)img.h, (UINT)img.w * 4, (UINT)img.px.size(),
                                      const_cast<BYTE*>(img.px.data()))) &&
            SUCCEEDED(fr->Commit()) &&
            SUCCEEDED(enc->Commit()))
            ok = true;
    }
    if (opt) opt->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

std::wstring UniquePath(const std::wstring& p) {
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    size_t dot = p.find_last_of(L'.'), sl = p.find_last_of(L"\\/");
    std::wstring base = p, ext;
    if (dot != std::wstring::npos && (sl == std::wstring::npos || dot > sl)) {
        base = p.substr(0, dot);
        ext  = p.substr(dot);
    }
    for (int i = 1; i < 1000; ++i) {
        std::wstring c = base + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) return c;
    }
    return p;
}

}  // namespace wv
