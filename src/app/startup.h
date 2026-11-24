// startup.h —— 与「进程怎么被启动」有关的两件事：开机自启、以管理员身份运行。
//
// 为什么需要单独一层：
//   · 权限低时按 Win+V 拿不到任务管理器 / 注册表编辑器这类高权限窗口的焦点，
//     热键看起来「失灵」——所以管理员模式是真实需求，不是洁癖；
//   · 提权只能用「重新以管理员启动自己」实现（进程权限不能在原地提升），
//     于是这里的函数大多带副作用：成功后调用方必须立刻退出。
//
// 开机自启的两种实现（SetAutostart / ReconcileAutostart 内部自己选）：
//   · 普通权限：HKCU\...\Run 写一个值，登录时由 explorer 拉起；
//   · 管理员模式：计划任务（登录触发 + 最高权限）。Run 项拉起的是普通权限进程，
//     那样每次登录都得弹一次 UAC，等于没做。
//   两者互斥。选中哪一种取决于「当前设置 + 当前账户是否属于管理员组」，见 DesiredAutostartForm。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

bool IsProcessElevated();

// 当前账户是否属于管理员组（不要求本进程已提权）。
// 标准用户既提不了权、也建不了「最高权限」的计划任务，界面上要据此禁用管理员模式。
bool IsUserInAdminGroup();

// 以管理员身份重新启动自身。成功返回 true —— 调用方应当马上退出让新实例接管。
bool RelaunchElevated();

// 按当前设置写（或删）开机自启。两种实现互斥，内部会先清掉另一种。
// 返回 false 表示期望的实现没建成；err 里是可以直接给用户看的说明
// （成功也可能有说明：比如「计划任务建不成，已退回普通自启」）。
bool SetAutostart(bool on, std::wstring* err = nullptr);

// 启动后对账：把实际的 Run 项 / 计划任务与「当前设置 + 当前权限」对齐。
// 每进程只调一次，且必须在提权判定之后（否则会把任务项改回 Run 项）。
// 用户开/关管理员模式后重启程序时，自启形式的切换就靠它完成。
void ReconcileAutostart();

// 启动留痕（%LOCALAPPDATA%\WinVClip\launch.log，只留最近几十行）。
// 「开机自启没生效」无法靠猜：这一行能确定进程到底有没有被拉起、起来的是什么权限。
void LogLaunch();

// 追加一行同格式的事件（单实例被挡、退出、自启对账失败…）。
// 有这一行才能区分「进程压根没起来」「起来了但立刻退出」和「退出后又被谁拉起来」。
void LogEvent(const std::wstring& line);
