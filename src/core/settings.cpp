// settings.cpp —— 实现见 settings.h。
#include "core/settings.h"

#include <cstdio>
#include <cstdlib>

#include "core/text_util.h"
#include "core/theme.h"

using namespace wv;

AppSettings g_cfg;

// ---------------------------------------------------------------- 路径
static std::wstring TrimSlash(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// 逐级创建目录。已存在时返回「是不是目录」，这样调用方只需要一个返回值。
static bool EnsureDir(const std::wstring& d) {
    if (d.empty()) return false;
    DWORD a = GetFileAttributesW(d.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t i = d.find_last_of(L"\\/");
    if (i != std::wstring::npos && i > 2) EnsureDir(d.substr(0, i));
    if (CreateDirectoryW(d.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0) return L".";
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return (k == std::wstring::npos) ? s : s.substr(0, k);
}

std::wstring DefaultTempDir() { return ExeDir() + L"\\temp"; }

std::wstring ConfigDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d;
    if (n > 0 && n < MAX_PATH) d = TrimSlash(std::wstring(buf, n));
    if (d.empty()) {
        if (!GetTempPathW(MAX_PATH, buf)) d = ExeDir();
        else d = TrimSlash(std::wstring(buf));
    }
    d += L"\\WinVClip";
    EnsureDir(d);
    return d;
}

std::wstring SettingsPath() { return ConfigDir() + L"\\settings.ini"; }

std::wstring TempDir() {
    static std::wstring cached;
    static std::wstring cachedWant;
    std::wstring want = g_cfg.tempDir.empty() ? DefaultTempDir() : TrimSlash(g_cfg.tempDir);
    if (!cached.empty() && cachedWant == want) return cached;
    std::wstring use = want;
    if (!EnsureDir(use)) {
        // 用户填的目录建不出来（盘符没了 / 无权限）：退回默认。
        // 默认（exe 目录\temp）在程序装进 Program Files 时也建不出来，
        // 那时再退到 %LOCALAPPDATA%\WinVClip\temp——总得有个能写的地方。
        use = (want == DefaultTempDir()) ? (ConfigDir() + L"\\temp") : DefaultTempDir();
        EnsureDir(use);
    }
    cached = use;
    cachedWant = want;
    return cached;
}

std::wstring TempImgDir() {
    std::wstring d = TempDir() + L"\\img";
    EnsureDir(d);
    return d;
}

std::wstring TempCacheDir() {
    std::wstring d = TempDir() + L"\\cache";
    EnsureDir(d);
    return d;
}

void TempClearImages() {
    std::wstring d = TempImgDir();
    if (d.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileW((d + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- 读写
static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

void SettingsLoad() {
    g_cfg = AppSettings{};
    FILE* fp = _wfopen(SettingsPath().c_str(), L"rb");
    if (!fp) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) all.append(buf, n);
    fclose(fp);

    size_t pos = 0;
    while (pos < all.size()) {
        size_t e = all.find('\n', pos);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq));
        std::string v = Trim(line.substr(eq + 1));   // 路径两端的空格按用户误输入处理

        if (k == "autostart")      g_cfg.autostart = (v == "1");
        else if (k == "admin")     g_cfg.admin = (v == "1");
        else if (k == "theme")     g_cfg.theme = atoi(v.c_str());
        else if (k == "tempdir")   g_cfg.tempDir = TrimSlash(U8N(v.c_str(), (int)v.size()));
        else if (k == "history")   g_cfg.historyLimit = atoi(v.c_str());
        else if (k == "confirmdel") g_cfg.confirmDelete = (v == "1");
    }
    if (g_cfg.theme < THEME_LIGHT || g_cfg.theme > THEME_SYSTEM) g_cfg.theme = THEME_SYSTEM;
    if (g_cfg.historyLimit < HIST_MIN) g_cfg.historyLimit = HIST_MIN;
    if (g_cfg.historyLimit > HIST_MAX) g_cfg.historyLimit = HIST_MAX;
}

void SettingsSave() {
    std::string out;
    auto put = [&](const char* k, const char* v) {
        out += k;
        out += '=';
        out += v;
        out += "\r\n";
    };
    put("autostart", g_cfg.autostart ? "1" : "0");
    put("admin", g_cfg.admin ? "1" : "0");
    out += "theme=" + std::to_string(g_cfg.theme) + "\r\n";
    out += "history=" + std::to_string(g_cfg.historyLimit) + "\r\n";
    out += "tempdir=" + W2U8(g_cfg.tempDir) + "\r\n";
    put("confirmdel", g_cfg.confirmDelete ? "1" : "0");

    FILE* fp = _wfopen(SettingsPath().c_str(), L"wb");
    if (!fp) return;
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
}

// ---------------------------------------------------------------- 主题
bool SystemPrefersDark() {
    DWORD v = 1;                 // 读不到就按浅色（Windows 的默认）
    DWORD cb = sizeof(v), type = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &cb) != ERROR_SUCCESS)
            v = 1;
        RegCloseKey(k);
    }
    return v == 0;
}

static void ApplyPalette(bool dark) {
    g_darkTheme = dark;
    if (dark) {
        C_CANVAS   = RGB(26, 28, 31);
        C_SURFACE  = RGB(35, 38, 43);
        C_SUBTLE   = RGB(45, 49, 55);
        C_INK      = RGB(236, 234, 230);
        C_MUTED    = RGB(154, 161, 168);
        C_LINE     = RGB(59, 64, 72);
        C_VOLT     = RGB(255, 122, 69);
        C_VOLTSOFT = RGB(74, 46, 34);
        C_SUCCESS  = RGB(63, 191, 133);
        C_DANGER   = RGB(226, 87, 76);
        C_RINGSOFT = RGB(58, 38, 30);
        C_SCRIM    = RGB(0, 0, 0);
    } else {
        C_CANVAS   = RGB(244, 242, 237);
        C_SURFACE  = RGB(255, 253, 248);
        C_SUBTLE   = RGB(231, 227, 219);
        C_INK      = RGB(32, 37, 43);
        C_MUTED    = RGB(116, 122, 125);
        C_LINE     = RGB(213, 208, 200);
        C_VOLT     = RGB(255, 107, 53);
        C_VOLTSOFT = RGB(255, 224, 210);
        C_SUCCESS  = RGB(47, 155, 104);
        C_DANGER   = RGB(216, 58, 58);
        C_RINGSOFT = RGB(255, 232, 221);
        C_SCRIM    = RGB(32, 37, 43);
    }
}

void ApplyThemeFromSettings() {
    bool dark = (g_cfg.theme == THEME_DARK) ||
                (g_cfg.theme == THEME_SYSTEM && SystemPrefersDark());
    ApplyPalette(dark);
}
