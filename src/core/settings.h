// settings.h —— 用户设置：唯一的读入口、唯一的落盘处。
//
// 为什么单独成模块：
//   这些设置会被几乎所有层读（绘制要看主题、采集要看历史上限、图片要看临时目录），
//   而写它们的地方只有一个（ui/actions 的设置窗口）。集中在这里，
//   「这个值从哪来、什么时候生效」就只有一个答案。
//
// 三条约定：
//   · 配置文件是 %LOCALAPPDATA%\WinVClip\settings.ini（不放 exe 目录：程序可能装在
//     Program Files 这类只读位置）。键值都按 UTF-8 存，路径可以带中文。
//   · 改完设置一律 SettingsSave()，别指望退出时统一写：本程序是被任务管理器
//     结束也不会打招呼的常驻进程。
//   · 临时目录用来放程序自己生成的内容：剪贴板图片的像素副本（<临时目录>\history\img，
//     历史持久化的一部分，不要清）和 emoji 图集这类可随时重建的缓存（<临时目录>\cache）。
//     <临时目录>\img 是旧版本的位置，启动时会被清一次。
//   · 历史与「最近使用」的落盘不在这里，见 core/history_store.h。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

// ---------------------------------------------------------------- 主题
enum { THEME_LIGHT = 0, THEME_DARK = 1, THEME_SYSTEM = 2 };

// ---------------------------------------------------------------- 历史上限
// 下限 50：再少就没有「历史」的意义了；上限 5000：g_items 是 deque，
// 每次采集都要线性扫一遍去重，条数太多时复制动作会开始有感。
enum { HIST_MIN = 50, HIST_MAX = 5000, HIST_STEP = 50, HIST_DEFAULT = 500 };

struct AppSettings {
    bool autostart = false;       // 开机自启动（写注册表 Run 项或计划任务）
    bool admin = false;           // 以管理员身份运行（启动时提权重启）
    int  theme = THEME_SYSTEM;    // THEME_*
    std::wstring tempDir;         // 空 = 默认 <exe 目录>\temp
    int  historyLimit = HIST_DEFAULT;
    // 删除单条历史（右键菜单「删除」/ Del 键）前是否先弹确认卡片。
    // 默认 true：删除不可撤销，误删一条复制的密码就没了。
    bool confirmDelete = true;
};

extern AppSettings g_cfg;

// ---------------------------------------------------------------- 路径
std::wstring ConfigDir();       // %LOCALAPPDATA%\WinVClip（配置文件、面板尺寸）
std::wstring SettingsPath();    // ConfigDir()\settings.ini
std::wstring DefaultTempDir();  // <exe 所在目录>\temp
std::wstring TempDir();         // 生效中的临时目录，保证存在（不可用时退回默认）
std::wstring TempImgDir();      // TempDir()\img    旧版本放图片副本的地方（只用于清理）
std::wstring TempCacheDir();    // TempDir()\cache  emoji 图集等可重建的缓存
void TempClearImages();         // 启动时清空 img 子目录（当前版本的图片副本在 history\img）

// ---------------------------------------------------------------- 读写
void SettingsLoad();
void SettingsSave();

// ---------------------------------------------------------------- 主题
bool SystemPrefersDark();        // 读系统「应用模式」：暗色 / 亮色
void ApplyThemeFromSettings();   // 按 g_cfg.theme 把调色板（theme.h 的 C_*）刷成对应色
