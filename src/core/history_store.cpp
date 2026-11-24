// history_store.cpp —— 实现见 history_store.h（那里写了为什么换成数据库、内存策略）。
#include "core/history_store.h"

#include <sqlite3.h>

#include <cstring>
#include <unordered_map>

#include "core/app_state.h"    // g_main / g_items / g_recentEmo / g_recentSym / TIMER_PERSIST
#include "core/io_png.h"       // ReadAllBytes / WriteAllBytes
#include "core/settings.h"     // TempDir / ConfigDir / HIST_MIN / HIST_MAX
#include "core/text_util.h"    // W2U8 / FileNameOf

using namespace wv;

// 正文留在内存里的字节上限：超过它的文本只从库里按需取（见 history_store.h 的内存策略）。
// 16KB 是「人一眼能看完」和「一整份日志」的分界；500 条历史若都按这个上限算，
// 短文本最多吃 8MB，而长正文再大也不进内存。
static const size_t kBodyInMemBytes = 16u * 1024;

namespace {

sqlite3* g_db = nullptr;

// ================================================================== 路径
bool EnsureDirW(const std::wstring& d) {
    if (d.empty()) return false;
    DWORD a = GetFileAttributesW(d.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t i = d.find_last_of(L"\\/");
    if (i != std::wstring::npos && i > 2) EnsureDirW(d.substr(0, i));
    if (CreateDirectoryW(d.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring HistoryDbPath() { return HistoryDir() + L"\\history.db"; }

// 老版本的单一二进制文件（全量重写那套）。只用于首次导入，导完改名 .old。
std::wstring LegacyDatPath() { return HistoryDir() + L"\\history.dat"; }

// 「上一次用的历史目录」记在配置目录（不随临时目录变），改设置后靠它找回旧数据
std::wstring DirMarkPath() { return ConfigDir() + L"\\history_dir.txt"; }

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadDirMark(std::wstring& out) {
    std::vector<BYTE> buf;
    if (!ReadAllBytes(DirMarkPath(), buf, 64 * 1024) || buf.empty()) return false;
    out = U8N((const char*)buf.data(), (int)buf.size());
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
    return !out.empty();
}

void WriteDirMark(const std::wstring& dir) {
    std::string u = W2U8(dir);
    WriteAllBytes(DirMarkPath(), u.data(), u.size());
}

// ================================================================== 数据库句柄
void Exec(const char* sql) {
    if (g_db) sqlite3_exec(g_db, sql, nullptr, nullptr, nullptr);
}

// 把库的 WAL 合并回主文件。搬库/关库前必须做一次：只搬 history.db 而把 -wal
// 丢掉，等于丢掉最后若干个**已提交**的事务（强杀后 -wal 里常有东西）。
void CheckpointDb(const std::wstring& path) {
    sqlite3* db = nullptr;
    std::string u = W2U8(path);
    if (sqlite3_open_v2(u.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

void CloseDb() {
    if (!g_db) return;
    sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    sqlite3_close(g_db);      // 所有 stmt 都已在各自函数里 finalize
    g_db = nullptr;
}

// 宽字符取值：全程用 sqlite3_*_text16 与 UTF-16 交互，
// 不做 UTF-8 往返，中文与 emoji 也就不存在转换损耗或半截代理对的问题。
std::wstring ColText16(sqlite3_stmt* s, int col) {
    const void* p = sqlite3_column_text16(s, col);
    int bytes = sqlite3_column_bytes16(s, col);
    if (!p || bytes <= 0) return std::wstring();
    return std::wstring((const wchar_t*)p, (size_t)bytes / sizeof(wchar_t));
}

void BindText16(sqlite3_stmt* s, int idx, const std::wstring& v) {
    // SQLITE_TRANSIENT：让 sqlite 自己拷一份，v 的生命周期就不用操心了
    sqlite3_bind_text16(s, idx, v.c_str(), (int)(v.size() * sizeof(wchar_t)),
                        SQLITE_TRANSIENT);
}

void BindSt(sqlite3_stmt* s, int idx, const SYSTEMTIME& st) {
    sqlite3_bind_blob(s, idx, &st, (int)sizeof(SYSTEMTIME), SQLITE_TRANSIENT);
}

bool ReadSt(sqlite3_stmt* s, int col, SYSTEMTIME& st) {
    const void* p = sqlite3_column_blob(s, col);
    if (!p || sqlite3_column_bytes(s, col) != (int)sizeof(SYSTEMTIME)) return false;
    memcpy(&st, p, sizeof(SYSTEMTIME));
    return true;
}

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS items("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  kind INTEGER NOT NULL,"
    "  st BLOB NOT NULL,"
    "  title TEXT NOT NULL DEFAULT '',"
    "  tag TEXT NOT NULL DEFAULT '',"
    "  source TEXT NOT NULL DEFAULT '',"
    "  text TEXT,"                      // 正文：大文本也放在这里，尺寸交给数据库
    "  img_path TEXT NOT NULL DEFAULT '',"
    "  is_png INTEGER NOT NULL DEFAULT 0,"
    "  img_w INTEGER NOT NULL DEFAULT 0,"
    "  img_h INTEGER NOT NULL DEFAULT 0,"
    "  hash INTEGER NOT NULL DEFAULT 0,"
    "  bytes INTEGER NOT NULL DEFAULT 0,"
    "  pinned INTEGER NOT NULL DEFAULT 0,"
    "  fav INTEGER NOT NULL DEFAULT 0,"
    "  is_code INTEGER NOT NULL DEFAULT 0,"
    "  is_link INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS files("
    "  item_id INTEGER NOT NULL,"
    "  ord INTEGER NOT NULL,"
    "  path TEXT NOT NULL,"
    "  PRIMARY KEY(item_id, ord)"
    ");"
    "CREATE TABLE IF NOT EXISTS recent("
    "  is_sym INTEGER NOT NULL,"
    "  ord INTEGER NOT NULL,"
    "  glyph TEXT NOT NULL,"
    "  name TEXT NOT NULL"
    ");";

bool OpenDb() {
    if (g_db) return true;
    std::string path = W2U8(HistoryDbPath());     // sqlite 在 Windows 上按 UTF-8 解释文件名
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    g_db = db;
    // WAL：提交只写增量，且强杀不会留下半截数据；synchronous=NORMAL 在 WAL 下已够安全
    // （最坏丢最后一个未提交事务，而事务本身是原子的）。这两条比自写 .tmp + 替换可靠。
    Exec("PRAGMA journal_mode=WAL;");
    Exec("PRAGMA synchronous=NORMAL;");
    if (sqlite3_exec(g_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        CloseDb();
        return false;
    }
    return true;
}

// 历史上限（含用户改小的情况）：加载时多出来的旧行会被下一次提交按「库里多出来的」删掉
int HistLimit() {
    int lim = g_cfg.historyLimit;
    if (lim < HIST_MIN) lim = HIST_MIN;
    if (lim > HIST_MAX) lim = HIST_MAX;
    return lim;
}

// files 表里该条目的路径列表（文件条目只有一条到几条，整表读一次比逐条查询便宜）
typedef std::unordered_map<int, std::vector<std::wstring>> FileMap;

void LoadFileMap(FileMap& out) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"SELECT item_id,path FROM files ORDER BY item_id,ord", -1, &s,
                             nullptr) != SQLITE_OK)
        return;
    while (sqlite3_step(s) == SQLITE_ROW) out[sqlite3_column_int(s, 0)].push_back(ColText16(s, 1));
    sqlite3_finalize(s);
}

// ================================================================== 写入
const char* kInsertItem =
    "INSERT INTO items(kind,st,title,tag,source,text,img_path,is_png,img_w,img_h,hash,bytes,"
    "pinned,fav,is_code,is_link) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)";

const char* kUpdateItem =
    "UPDATE items SET kind=?2,st=?3,title=?4,tag=?5,source=?6,img_path=?7,is_png=?8,img_w=?9,"
    "img_h=?10,hash=?11,bytes=?12,pinned=?13,fav=?14,is_code=?15,is_link=?16,"
    // ?17 为 NULL 表示「内存里没有正文，保持库里那份」——加载来的长文本就是这样，
    // 否则每次提交都会把正文写成空串，一条大文本就没了。
    "text=CASE WHEN ?17 IS NULL THEN text ELSE ?17 END "
    "WHERE id=?1";

// 新条目入库，成功时回填 dbId。调用方负责事务。
bool InsertItem(ClipItem& it) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(g_db, kInsertItem, -1, &s, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(s, 1, it.kind);
    BindSt(s, 2, it.st);
    BindText16(s, 3, it.title);
    BindText16(s, 4, it.tag);
    BindText16(s, 5, it.source);
    if (it.text.empty()) sqlite3_bind_null(s, 6);
    else BindText16(s, 6, it.text);
    BindText16(s, 7, it.diskPath);
    sqlite3_bind_int(s, 8, it.isPng ? 1 : 0);
    sqlite3_bind_int(s, 9, it.imgW);
    sqlite3_bind_int(s, 10, it.imgH);
    sqlite3_bind_int64(s, 11, (sqlite3_int64)it.hash);
    sqlite3_bind_int64(s, 12, (sqlite3_int64)it.bytes);
    sqlite3_bind_int(s, 13, it.pinned ? 1 : 0);
    sqlite3_bind_int(s, 14, it.fav ? 1 : 0);
    sqlite3_bind_int(s, 15, it.isCode ? 1 : 0);
    sqlite3_bind_int(s, 16, it.isLink ? 1 : 0);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok) return false;
    it.dbId = (int)sqlite3_last_insert_rowid(g_db);

    if (it.kind == 1) {     // 文件条目的路径列表只在入库时写一次（之后不会变）
        sqlite3_stmt* f = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT INTO files(item_id,ord,path) VALUES(?1,?2,?3)", -1, &f,
                               nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < it.files.size(); ++i) {
                sqlite3_reset(f);
                sqlite3_bind_int(f, 1, it.dbId);
                sqlite3_bind_int(f, 2, (int)i);
                BindText16(f, 3, it.files[i]);
                sqlite3_step(f);
            }
            sqlite3_finalize(f);
        }
    }
    return true;
}

// 已有条目更新（只写元数据与标志位；正文见 kUpdateItem 的 ?17 说明）
void UpdateItem(const ClipItem& it) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(g_db, kUpdateItem, -1, &s, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(s, 1, it.dbId);
    sqlite3_bind_int(s, 2, it.kind);
    BindSt(s, 3, it.st);
    BindText16(s, 4, it.title);
    BindText16(s, 5, it.tag);
    BindText16(s, 6, it.source);
    BindText16(s, 7, it.diskPath);
    sqlite3_bind_int(s, 8, it.isPng ? 1 : 0);
    sqlite3_bind_int(s, 9, it.imgW);
    sqlite3_bind_int(s, 10, it.imgH);
    sqlite3_bind_int64(s, 11, (sqlite3_int64)it.hash);
    sqlite3_bind_int64(s, 12, (sqlite3_int64)it.bytes);
    sqlite3_bind_int(s, 13, it.pinned ? 1 : 0);
    sqlite3_bind_int(s, 14, it.fav ? 1 : 0);
    sqlite3_bind_int(s, 15, it.isCode ? 1 : 0);
    sqlite3_bind_int(s, 16, it.isLink ? 1 : 0);
    if (it.text.empty()) sqlite3_bind_null(s, 17);
    else BindText16(s, 17, it.text);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// 库里存在、内存里已经没了的行（删除条目、历史上限裁剪都靠它）
void DeleteMissingItems() {
    std::string sql = "DELETE FROM items";
    if (!g_items.empty()) {
        sql += " WHERE id NOT IN (";
        for (size_t i = 0; i < g_items.size(); ++i) {
            if (g_items[i].dbId <= 0) continue;      // 还没入库的不参与
            sql += (sql.back() == '(' ? "" : ",");
            sql += std::to_string(g_items[i].dbId);
        }
        if (sql.back() == '(') return;               // 没有任何已入库的条目
        sql += ")";
    }
    Exec(sql.c_str());
    Exec("DELETE FROM files WHERE item_id NOT IN (SELECT id FROM items)");
}

void SaveRecent() {
    Exec("DELETE FROM recent");
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(g_db, "INSERT INTO recent(is_sym,ord,glyph,name) VALUES(?1,?2,?3,?4)",
                           -1, &s, nullptr) != SQLITE_OK)
        return;
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<RecentGlyph>& src = pass ? g_recentSym : g_recentEmo;
        for (size_t i = 0; i < src.size(); ++i) {
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, pass);
            sqlite3_bind_int(s, 2, (int)i);
            sqlite3_bind_text(s, 3, src[i].g.c_str(), (int)src[i].g.size(), SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, src[i].n.c_str(), (int)src[i].n.size(), SQLITE_TRANSIENT);
            sqlite3_step(s);
        }
    }
    sqlite3_finalize(s);
}

// 提交成功后：超过阈值的正文已在库里，可以把它从内存放掉了。
// 这就是「常驻时的内存不随历史内容增长」的落点，必须在 COMMIT 之后做——
// 提交前放掉，那一条的正文就再也没机会写进库。
void ReleaseLongTexts() {
    for (auto& it : g_items) {
        // 判据用 it.bytes（UTF-8 字节数），与加载时那条 SQL 的 bytes<=?1 同一口径；
        // 这里若拿 text.size() 算，中文的字节数与库里的口径对不上。
        if (it.kind == 0 && it.bytes > kBodyInMemBytes) std::wstring().swap(it.text);
    }
}

}  // namespace

// ================================================================== 图片副本与孤儿清理
std::wstring HistoryDir() {
    std::wstring d = TempDir() + L"\\history";
    EnsureDirW(d);
    return d;
}

std::wstring HistoryImgDir() {
    std::wstring d = HistoryDir() + L"\\img";
    EnsureDirW(d);
    return d;
}

std::wstring StoreImageCopy(const std::vector<BYTE>& bits) {
    // 文件名带「进程启动时的 GetTickCount64 + 序号」：pid 会被复用，
    // 只用 pid 的话新会话可能覆盖旧会话留下的图片。
    static ULONGLONG s_epoch = 0;
    static unsigned s_seq = 0;
    std::wstring dir = HistoryImgDir();
    if (dir.empty() || bits.empty()) return L"";
    if (!s_epoch) s_epoch = GetTickCount64() | 1;
    wchar_t nm[80];
    wsprintfW(nm, L"\\img-%I64u-%u.bin", s_epoch, ++s_seq);
    std::wstring p = dir + nm;
    if (!WriteAllBytes(p, bits.data(), bits.size())) return L"";
    return p;
}

// 程序被强杀时可能留下「历史里已经没有、img 目录里还在」的像素文件。
// 加载完成后扫一次目录，把没人引用的删掉。
// 只比较文件名：StoreImageCopy 生成的名字带进程启动时刻 + 序号，全局唯一。
static void PruneOrphanImages() {
    std::wstring dir = HistoryImgDir();
    if (dir.empty()) return;
    std::vector<std::wstring> keep;
    for (const auto& it : g_items)
        if (!it.diskPath.empty()) keep.push_back(FileNameOf(it.diskPath));

    std::vector<std::wstring> dead;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        bool used = false;
        for (const auto& k : keep) if (k == name) { used = true; break; }
        if (!used) dead.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& name : dead) DeleteFileW((dir + L"\\" + name).c_str());
}

// ================================================================== 旧格式导入（一次性）
// 老版本的 history.dat：整个历史是一个自定义二进制包，每次改动全量重写。
// 这里保留它当初的 Reader 只为了把用户已有的历史导进数据库，之后这条路径就再不会走到。
namespace {

const char kMagic[4] = {'W', 'V', 'C', 'H'};
const unsigned kVersion = 1;
enum : unsigned { F_PIN = 1, F_FAV = 2, F_PNG = 4, F_CODE = 8, F_LINK = 16 };

struct Reader {
    const char* p = nullptr;
    size_t n = 0, i = 0;
    bool bad = false;
    bool raw(void* o, size_t k) {
        if (bad || i + k > n) { bad = true; return false; }
        memcpy(o, p + i, k);
        i += k;
        return true;
    }
    unsigned u8v() { unsigned char v = 0; raw(&v, 1); return v; }
    unsigned u32v() { unsigned v = 0; raw(&v, sizeof v); return v; }
    unsigned long long u64v() { unsigned long long v = 0; raw(&v, sizeof v); return v; }
    std::string str() {
        unsigned k = u32v();
        if (bad || i + k > n) { bad = true; return std::string(); }
        std::string s(p + i, k);
        i += k;
        return s;
    }
    std::wstring wstr() {
        std::string s = str();
        return bad ? std::wstring() : U8N(s.data(), (int)s.size());
    }
};

bool ReadLegacyItem(Reader& r, ClipItem& it) {
    it.kind = (int)r.u8v();
    unsigned f = r.u8v();
    it.pinned = (f & F_PIN) != 0;
    it.fav    = (f & F_FAV) != 0;
    it.isPng  = (f & F_PNG) != 0;
    it.isCode = (f & F_CODE) != 0;
    it.isLink = (f & F_LINK) != 0;
    if (!r.raw(&it.st, sizeof(SYSTEMTIME))) return false;
    it.title = r.wstr();
    it.tag = r.wstr();
    it.source = r.wstr();
    it.text = r.wstr();
    it.diskPath = r.wstr();
    unsigned nf = r.u32v();
    if (r.bad || nf > 20000) return false;
    it.files.reserve(nf);
    for (unsigned i = 0; i < nf; ++i) {
        std::wstring s = r.wstr();
        if (r.bad) return false;
        it.files.push_back(std::move(s));
    }
    it.imgW = (int)r.u32v();
    it.imgH = (int)r.u32v();
    it.hash = (size_t)r.u64v();
    it.bytes = (size_t)r.u64v();
    if (r.bad) return false;
    if (it.kind < 0 || it.kind > 2) return false;
    if (it.title.empty()) {
        it.title = (it.kind == 2) ? (L"图片 " + std::to_wstring(it.imgW) + L" × " +
                                     std::to_wstring(it.imgH))
                                 : (it.kind == 1 ? L"(文件)" : L"(文本)");
    }
    return true;
}

// 把老文件读出来写进数据库。任何一步不成立就整体放弃（原文件留着，不会被破坏）。
void ImportLegacyDat() {
    std::vector<BYTE> buf;
    if (!ReadAllBytes(LegacyDatPath(), buf, 256u * 1024 * 1024) || buf.size() < 12) return;
    Reader r{(const char*)buf.data(), buf.size()};
    char magic[4]{};
    if (!r.raw(magic, 4) || memcmp(magic, kMagic, 4) != 0 || r.u32v() != kVersion) return;
    unsigned n = r.u32v();
    if (r.bad || n > 200000) return;

    std::deque<ClipItem> items;
    for (unsigned i = 0; i < n && !r.bad; ++i) {
        ClipItem it;
        if (!ReadLegacyItem(r, it)) { r.bad = true; break; }
        items.push_back(std::move(it));
    }
    if (r.bad) return;

    std::vector<RecentGlyph> emo, sym;
    unsigned ne = r.u32v();
    for (unsigned i = 0; i < ne && !r.bad && i < 500; ++i) {
        RecentGlyph g;
        g.g = r.str();
        g.n = r.str();
        if (!r.bad) emo.push_back(std::move(g));
    }
    unsigned ns = r.u32v();
    for (unsigned i = 0; i < ns && !r.bad && i < 500; ++i) {
        RecentGlyph g;
        g.g = r.str();
        g.n = r.str();
        if (!r.bad) sym.push_back(std::move(g));
    }

    if (!OpenDb()) return;
    Exec("BEGIN IMMEDIATE");
    // 老文件是「最新在前」的顺序，按倒序插入，让 id 的先后与新采集的顺序一致
    for (auto it = items.rbegin(); it != items.rend(); ++it) InsertItem(*it);
    std::vector<RecentGlyph> keepEmo = g_recentEmo, keepSym = g_recentSym;
    g_recentEmo = emo;
    g_recentSym = sym;
    SaveRecent();
    g_recentEmo = keepEmo;
    g_recentSym = keepSym;
    Exec("COMMIT");

    // 导入完改名保留：用户的旧历史值得留一手回退，也不能因为一次导入失败就没了
    MoveFileExW(LegacyDatPath().c_str(), (LegacyDatPath() + L".old").c_str(),
                MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

// ================================================================== 读写入口
// 定义在本节之后（HistoryFlush 之后），这里先声明：HistoryLoad 也要用它。
static void MoveDataFromDir(const std::wstring& dir);

void HistoryLoad() {
    // 用户改过「临时文件目录」：把上一份数据取到当前目录来。
    // 图片文件不搬（diskPath 是绝对路径，旧文件照样读得出来）。
    std::wstring mark;
    if (ReadDirMark(mark) && mark != HistoryDir() && !FileExists(HistoryDbPath()))
        MoveDataFromDir(mark);
    WriteDirMark(HistoryDir());

    if (!FileExists(HistoryDbPath()) && FileExists(LegacyDatPath())) ImportLegacyDat();
    if (!OpenDb()) { PruneOrphanImages(); return; }

    FileMap fileMap;
    LoadFileMap(fileMap);

    std::deque<ClipItem> items;
    sqlite3_stmt* s = nullptr;
    const wchar_t* sql =
        L"SELECT id,kind,st,title,tag,source,"
        // 正文只取「够短」的那些：长文本留在库里，用时再取（见 kBodyInMemBytes）
        L"CASE WHEN bytes<=?1 THEN text ELSE NULL END,img_path,is_png,img_w,img_h,hash,bytes,"
        L"pinned,fav,is_code,is_link FROM items "
        // 置顶 / 收藏的条目必须全部载入：它们不占历史上限的名额（见 capture.cpp 的
        // ApplyHistoryLimit），所以 LIMIT 只能作用在普通条目上。漏载的后果不只是
        // 「看不到」——HistoryFlush 的 DeleteMissingItems 会把库里「内存里没有的」
        // 行删掉，等于静默删掉用户的收藏。
        L"WHERE pinned=1 OR fav=1 OR id IN "
        L"(SELECT id FROM items WHERE pinned=0 AND fav=0 ORDER BY id DESC LIMIT ?2) "
        L"ORDER BY id DESC";
    if (sqlite3_prepare16_v2(g_db, sql, -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)kBodyInMemBytes);
        sqlite3_bind_int(s, 2, HistLimit());
        while (sqlite3_step(s) == SQLITE_ROW) {
            ClipItem it;
            it.dbId   = sqlite3_column_int(s, 0);
            it.kind   = sqlite3_column_int(s, 1);
            if (!ReadSt(s, 2, it.st)) continue;
            it.title  = ColText16(s, 3);
            it.tag    = ColText16(s, 4);
            it.source = ColText16(s, 5);
            if (sqlite3_column_type(s, 6) != SQLITE_NULL) it.text = ColText16(s, 6);
            it.diskPath = ColText16(s, 7);
            it.isPng  = sqlite3_column_int(s, 8) != 0;
            it.imgW   = sqlite3_column_int(s, 9);
            it.imgH   = sqlite3_column_int(s, 10);
            it.hash   = (size_t)sqlite3_column_int64(s, 11);
            it.bytes  = (size_t)sqlite3_column_int64(s, 12);
            it.pinned = sqlite3_column_int(s, 13) != 0;
            it.fav    = sqlite3_column_int(s, 14) != 0;
            it.isCode = sqlite3_column_int(s, 15) != 0;
            it.isLink = sqlite3_column_int(s, 16) != 0;
            if (it.kind == 1) {
                auto fit = fileMap.find(it.dbId);
                if (fit != fileMap.end()) it.files = fit->second;
            }
            items.push_back(std::move(it));
        }
        sqlite3_finalize(s);
    }
    g_items = std::move(items);

    // 表情 / 符号的「最近使用」：只是原始记录，条目索引由 RebuildRecentItems 惰性重建
    std::vector<RecentGlyph> emo, sym;
    if (sqlite3_prepare_v2(g_db, "SELECT is_sym,glyph,name FROM recent ORDER BY is_sym,ord", -1, &s,
                           nullptr) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            RecentGlyph g;
            const void* gp = sqlite3_column_text(s, 1);
            int gl = sqlite3_column_bytes(s, 1);
            const void* np = sqlite3_column_text(s, 2);
            int nl = sqlite3_column_bytes(s, 2);
            if (gp && gl > 0) g.g.assign((const char*)gp, (size_t)gl);
            if (np && nl > 0) g.n.assign((const char*)np, (size_t)nl);
            (sqlite3_column_int(s, 0) ? sym : emo).push_back(std::move(g));
        }
        sqlite3_finalize(s);
    }
    g_recentEmo = std::move(emo);
    g_recentSym = std::move(sym);
    g_recentEmoItems.clear();
    g_recentSymItems.clear();
    g_recentCount[0] = g_recentCount[1] = 0;

    PruneOrphanImages();
}

void HistoryFlush() {
    if (!OpenDb()) return;

    // 图片条目若只有内存里的像素（上一次提交失败过），这里补一份文件，
    // 否则重启后这一条就只剩个空壳。
    for (auto& it : g_items)
        if (it.kind == 2 && it.diskPath.empty() && !it.bits.empty())
            it.diskPath = StoreImageCopy(it.bits);

    Exec("BEGIN IMMEDIATE");
    DeleteMissingItems();
    for (auto& it : g_items) {
        if (it.dbId <= 0) InsertItem(it);
        else UpdateItem(it);
    }
    SaveRecent();
    Exec("COMMIT");

    ReleaseLongTexts();      // 正文已进库，长文本从内存放掉
}

void HistoryTouch() {
    if (g_main) SetTimer(g_main, TIMER_PERSIST, 900, nullptr);
}

// 把某个目录里的数据搬进当前目录。
//   · 有 history.db → 连带 -wal/-shm 一起搬（只搬 .db 会丢掉 WAL 里还没合并的事务）；
//   · 只有老 history.dat → 搬它，HistoryLoad 会导入。
static void MoveDataFromDir(const std::wstring& dir) {
    std::wstring oldDb = dir + L"\\history.db";
    if (FileExists(oldDb)) {
        if (!FileExists(HistoryDbPath())) {
            CheckpointDb(oldDb);      // 先把它的 WAL 合并进去，否则搬过去的就是旧快照
            MoveFileW(oldDb.c_str(), HistoryDbPath().c_str());
        }
        DeleteFileW((oldDb + L"-wal").c_str());
        DeleteFileW((oldDb + L"-shm").c_str());
        return;
    }
    std::wstring oldDat = dir + L"\\history.dat";
    if (FileExists(oldDat) && !FileExists(LegacyDatPath()))
        MoveFileW(oldDat.c_str(), LegacyDatPath().c_str());
}

void HistoryMigrateFrom(const std::wstring& oldDir) {
    if (oldDir.empty() || oldDir == HistoryDir()) return;
    CloseDb();               // 搬文件前必须放开句柄
    MoveDataFromDir(oldDir);
    WriteDirMark(HistoryDir());
    OpenDb();
}

bool EnsureItemText(ClipItem& it) {
    if (!it.text.empty()) return true;
    if (it.dbId <= 0 || !OpenDb()) return false;
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    if (sqlite3_prepare16_v2(g_db, L"SELECT text FROM items WHERE id=?1", -1, &s, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int(s, 1, it.dbId);
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL) {
            it.text = ColText16(s, 0);
            ok = !it.text.empty();
        }
        sqlite3_finalize(s);
    }
    return ok;
}

void HistorySearchBodies(const std::vector<int>& ids, const std::wstring& query,
                         std::vector<int>& hitDbIds) {
    if (ids.empty() || query.empty() || !OpenDb()) return;
    sqlite3_stmt* s = nullptr;
    // instr 而不是 LIKE：省掉 % _ \ 的转义（查询内容完全由用户决定，转义漏一处就搜错）。
    // lower() 只处理 ASCII，与内存里那套 Lower()+find 对中文的表现一致。
    if (sqlite3_prepare16_v2(g_db, L"SELECT id FROM items WHERE id=?1 AND instr(lower(text),?2)>0",
                             -1, &s, nullptr) != SQLITE_OK)
        return;
    for (int id : ids) {
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, id);
        BindText16(s, 2, query);
        if (sqlite3_step(s) == SQLITE_ROW) hitDbIds.push_back(id);
    }
    sqlite3_finalize(s);
}
