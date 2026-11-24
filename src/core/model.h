// 核心数据结构：剪贴板条目与字形库条目。
//
// 只放「字段 + 极少量不变量」，不放任何函数逻辑，
// 因此任何模块都可以依赖它，它不依赖任何业务模块（依赖单向的最底层）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>

#include <memory>
#include <string>
#include <vector>

// 列表里的图片缩略图（惰性构建，ClipItem 之间靠 shared_ptr 共享：
// 去重时整条被复制，缓存必须跟着内容一起走，不能有裸指针）
struct Thumb {
    ID2D1Bitmap* bmp = nullptr;
    int w = 0, h = 0;                 // 位图实际像素尺寸（保持原图宽高比）
    ~Thumb() { if (bmp) bmp->Release(); }
};
typedef std::shared_ptr<Thumb> ThumbRef;

struct ClipItem {
    int kind = 0;                        // 0=文本 1=文件 2=图片
    // 数据库主键（见 core/history_store.h）。0 = 还没入库的新条目。
    // 由持久化层独占写入，别处只读：它决定了「这一条是 INSERT 还是 UPDATE」。
    int dbId = 0;
    std::wstring title;                  // 单行摘要
    std::wstring tag;                    // 类型标签：文本/代码/链接/图片/文件
    std::wstring text;                   // kind 0
    std::vector<std::wstring> files;     // kind 1
    std::vector<BYTE> bits;              // kind 2
    // kind 2 的像素副本在临时目录里的路径（设置里的「临时文件目录」）。
    // 内存里的 bits 会按预算释放（只留最近几张），需要真内容时用 EnsureItemBits 读回；
    // 文件条目（kind 1）不复制内容，只引用原路径——那是用户自己的文件。
    std::wstring diskPath;
    bool isPng = false;
    int imgW = 0, imgH = 0;
    // kind==2 时 bits 的内容哈希：去重时先比它。
    // 图片去重原来对每条历史做一次全量 memcmp，而同类截图（同分辨率 DIB）size 常常完全一样，
    // 于是每次复制都要比几千万字节 ×最多 60 条 —— 那是复制后界面卡一下的来源。
    size_t hash = 0;
    SYSTEMTIME st{};
    std::wstring source;                 // 来源窗口
    bool pinned = false, fav = false;
    bool isCode = false, isLink = false;
    size_t bytes = 0;                    // 内容字节数（列表里显示大小）
    ThumbRef thumb;                      // kind==2 的缩略图缓存
};

// ---------------------------------------------------------------- 字形库（表情 / 符号）
// 数据在 src/glyph_data.cpp（构建期由参考页面的 symbols.js / emoji.js 生成）。
// 运行期的取舍：
//   · 启动只解析「分类表」（60 来行），不碰几千条条目；
//   · 条目索引按分类惰性建立，且只建当前正在看的那一类；
//   · 名称全程保持 UTF-8，仅在真正要画到屏幕时才转宽字符。
// 9000 多条数据若在启动时统一转成 std::wstring，光分配就够卡一帧。
//
// GItem 里的 const char* 指向 glyph_data.cpp 的静态 blob，生命周期等于进程，
// 所以只存裸指针 + 长度，不拷贝。
struct GItem {
    const char* g = "";   int gl = 0;    // 字形
    const char* n = "";   int nl = 0;    // 中文名
    const char* n2 = "";  int n2l = 0;   // 英文名（表情有，符号空）
};

struct GCat {
    std::string key, name;               // UTF-8
    const char* raw = nullptr;           // 本分类条目在 blob 中的范围
    const char* rawEnd = nullptr;
    int count = 0;
    bool loaded = false;
    std::vector<GItem> items;            // 惰性索引
};

struct RecentGlyph { std::string g, n; };  // 最近使用：字形 + 名称（UTF-8）
