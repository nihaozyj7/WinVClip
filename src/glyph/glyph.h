// glyph.h —— 字形库（表情 / 符号）的解析、索引与可画性判定。
//
// 边界：只管「数据」——从 glyph_data.cpp 的静态 blob 里解析分类、按需建条目索引、
// 判断某个字形当前字体环境画不画得出来。不碰布局、不碰绘制、不开线程。
// 唯一的对外可变依赖是 app_state.h 里的字形库全局（分类表、最近使用、命中缓存），
// 这些由本模块独占写入。
#pragma once

#include <string>
#include <vector>

#include "core/app_state.h"   // GCat / GItem / RecentGlyph 与字形库全局
#include "core/model.h"

// ---------------------------------------------------------------- 解析
// 启动时只解析分类表（不碰几千条条目），条目索引在真正要显示某类时才建。
void GlyphParseTable(const char* blob, bool sym);
void LoadCat(GCat& c, int page);

// ---------------------------------------------------------------- 分类
int  GlyphCatTotal(int page);
GCat& GlyphCat(int page, int idx);            // idx 从 1 开始，0 是「最近」
int  GlyphCatIndexByKey(int page, const char* key);
std::wstring GlyphCatName(int page, int idx);
int  GlyphCatCount(int page, int idx);
bool RecentEmpty(int page);                   // 「最近」为空时整条分类都不该出现
int  CurGlyphCat(int page);
void SetGlyphCat(int page, int idx);
void RebuildRecentItems(bool sym);

// ---------------------------------------------------------------- 查询
bool GlyphSearching(int page);
const std::vector<GItem>& GlyphItems(int page);
int  GlyphCount(int page);
void RebuildGlyphHits();
void AddRecentGlyph(int page, const GItem& it);

// ---------------------------------------------------------------- 可画性
// 系统字体画不出来的字形（显示成方框）不该占格子，建索引时就剔掉。
// 判定走 DirectWrite 的 IDWriteFont::HasCharacter（GDI 的 GetGlyphIndicesW
// 按 UTF-16 码元算，代理对一律误判缺字）。结果按字形缓存，实际只算一次。
bool GlyphDrawable(int page, const char* g, int gl);
// 单个字体能否画出整串：PickSymFont 用它挑符号兜底字体。
bool GlyphFontCovers(IDWriteFont* f, const std::wstring& s);
// 字体环境变了（DPI/字体替换）时作废可画性缓存。
void ClearDrawCache();
