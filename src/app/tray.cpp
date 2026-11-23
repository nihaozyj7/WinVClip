// tray.cpp —— 实现见 tray.h。
#include "app/tray.h"

#include "core/app_state.h"   // g_nid / g_main
#include "core/theme.h"

using namespace wv;

// ================================================================== 托盘
void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"WinVClip —— Win+V 自定义剪贴板");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

void RemoveTray() {
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
}