// startup.cpp —— 实现见 startup.h。
#include "app/startup.h"

#include <shellapi.h>   // ShellExecuteW（runas）
#include <tlhelp32.h>   // 父进程 pid/名字（启动日志用）
#include <sddl.h>       // ConvertSidToStringSidW（任务 ACL 要当前用户 SID）
#include <initguid.h>   // 让 taskschd.h 里的 CLSID_TaskScheduler / IID_* 成为实体定义
#include <taskschd.h>
#include <cstdio>
#include <string>
#include <vector>

#include "core/settings.h"
#include "core/text_util.h"

using namespace wv;

static const wchar_t* kRunKey   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"WinVClip";
static const wchar_t* kTaskName = L"WinVClip";

static std::wstring ExePath() {
    wchar_t p[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    return (n == 0) ? std::wstring() : std::wstring(p, n);
}

bool IsProcessElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = sizeof(el);
    bool elevated = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n) && el.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

// 注意这里不能复用 IsProcessElevated：非提权的管理员令牌里 Administrators 组是
// deny-only，隶属检查照样能查到「这个账户属于管理员组」——正是我们要的信息。
bool IsUserInAdminGroup() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD len = 0;
    GetTokenInformation(tok, TokenGroups, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 1);
    bool in = false;
    if (len && GetTokenInformation(tok, TokenGroups, buf.data(), len, &len)) {
        BYTE adm[SECURITY_MAX_SID_SIZE]{};
        DWORD an = sizeof(adm);
        if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adm, &an)) {
            auto* tg = (TOKEN_GROUPS*)buf.data();
            for (DWORD i = 0; i < tg->GroupCount; ++i) {
                if (tg->Groups[i].Sid && EqualSid(tg->Groups[i].Sid, adm)) { in = true; break; }
            }
        }
    }
    CloseHandle(tok);
    return in;
}

bool RelaunchElevated() {
    std::wstring exe = ExePath();
    if (exe.empty()) return false;
    // --elevated 是给新实例看的：它要靠这个标记知道「有个旧实例正在退出，先等锁」，
    // 否则单实例互斥会让它以为重复启动，直接退出（见 main.cpp）。
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", exe.c_str(), L"--elevated", nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// ================================================================== COM 小工具
namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};

// BSTR 的 RAII 包装：任务计划的接口全是 BSTR 参数，随手 SysAllocString 会漏。
struct Bstr {
    BSTR v = nullptr;
    explicit Bstr(const wchar_t* s) { v = SysAllocString(s ? s : L""); }
    ~Bstr() { if (v) SysFreeString(v); }
    operator BSTR() const { return v; }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

VARIANT VarEmpty() {
    VARIANT v;
    VariantInit(&v);
    return v;
}

VARIANT VarBstr(const wchar_t* s) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(s ? s : L"");
    return v;
}

// 当前用户 SID 的字符串形式（S-1-5-21-...），用于任务 ACL。
std::wstring CurrentUserSid() {
    std::wstring sid;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return sid;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 1);
    if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        auto* tu = (TOKEN_USER*)buf.data();
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &s) && s) {
            sid = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return sid;
}

std::wstring HresultText(HRESULT hr) {
    wchar_t* m = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)hr, 0, (LPWSTR)&m, 0, nullptr);
    std::wstring s;
    if (n && m) {
        s.assign(m, n);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    }
    if (m) LocalFree(m);
    if (s.empty()) {
        wchar_t b[32];
        std::swprintf(b, 32, L"0x%08X", (unsigned)hr);
        s = b;
    }
    return s;
}

std::wstring CurrentUser() {
    wchar_t user[256]{};
    DWORD n = 256;
    if (!GetUserNameW(user, &n)) return std::wstring();
    wchar_t dom[256]{};
    if (GetEnvironmentVariableW(L"USERDOMAIN", dom, 256) == 0) return std::wstring(user);
    return std::wstring(dom) + L"\\" + user;
}

// 打开任务计划根目录。调用方负责已经 CoInitialize 过。
bool OpenRoot(Com<ITaskService>& svc, Com<ITaskFolder>& root, std::wstring* err) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  (void**)&svc.p);
    if (FAILED(hr)) {
        if (err) *err = L"任务计划服务不可用：" + HresultText(hr);
        return false;
    }
    VARIANT e1 = VarEmpty(), e2 = VarEmpty(), e3 = VarEmpty(), e4 = VarEmpty();
    hr = svc->Connect(e1, e2, e3, e4);
    VariantClear(&e1); VariantClear(&e2); VariantClear(&e3); VariantClear(&e4);
    if (FAILED(hr)) {
        if (err) *err = L"连接任务计划服务失败：" + HresultText(hr);
        return false;
    }
    hr = svc->GetFolder(Bstr(L"\\"), &root.p);
    if (FAILED(hr) || !root) {
        if (err) *err = L"打开任务计划根目录失败：" + HresultText(hr);
        return false;
    }
    return true;
}

}  // namespace

// ================================================================== 计划任务
// 为什么不用 schtasks.exe：它的命令行没有任何参数能改
//   · DisallowStartIfOnBatteries（默认 true）—— 笔记本用电池开机时任务被静默跳过，
//     日志里只有一句 "task did not run because computer is running on battery"，用户侧表现为「自启没生效」；
//   · ExecutionTimeLimit（默认 72 小时）—— 到点直接 End 掉进程，常驻程序会莫名消失；
//   · StartWhenAvailable、托盘就绪前的延迟等等。
// Task Scheduler 的 COM 接口能把这些全设对，且不用临时文件、不用解析中文路径。
static bool CreateLogonTask(const std::wstring& exe, std::wstring* err) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        if (err) *err = L"COM 初始化失败：" + HresultText(hrCo);
        return false;
    }
    const bool owned = (hrCo == S_OK || hrCo == S_FALSE);   // RPC_E_CHANGED_MODE 是别人初始化的，不归我们注销

    bool ok = false;
    {
        Com<ITaskService> svc;
        Com<ITaskFolder> root;
        if (!OpenRoot(svc, root, err)) { if (owned) CoUninitialize(); return false; }

        Com<ITaskDefinition> def;
        HRESULT hr = svc->NewTask(0, &def.p);
        if (FAILED(hr) || !def) {
            if (err) *err = L"创建任务定义失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }

        const std::wstring user = CurrentUser();

        Com<IRegistrationInfo> ri;
        if (SUCCEEDED(def->get_RegistrationInfo(&ri.p)) && ri) {
            ri->put_Author(Bstr(user.c_str()));
            ri->put_Description(Bstr(L"WinVClip —— 登录时以管理员身份启动 Win+V 剪贴板面板"));
        }

        Com<ITaskSettings> st;
        if (SUCCEEDED(def->get_Settings(&st.p)) && st) {
            st->put_Enabled(VARIANT_TRUE);
            st->put_StartWhenAvailable(VARIANT_TRUE);      // 错过登录触发（比如睡眠中）也补跑
            st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            st->put_ExecutionTimeLimit(Bstr(L"PT0S"));     // 不限时长，否则 72 小时后进程被结束
            st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
            st->put_AllowDemandStart(VARIANT_TRUE);
            st->put_RunOnlyIfIdle(VARIANT_FALSE);
            st->put_AllowHardTerminate(VARIANT_TRUE);
        }

        Com<IPrincipal> pr;
        if (SUCCEEDED(def->get_Principal(&pr.p)) && pr) {
            pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            pr->put_RunLevel(TASK_RUNLEVEL_HIGHEST);       // 登录即以管理员起，不再弹 UAC
            if (!user.empty()) pr->put_UserId(Bstr(user.c_str()));
        }

        Com<ITriggerCollection> trs;
        hr = def->get_Triggers(&trs.p);
        if (FAILED(hr) || !trs) {
            if (err) *err = L"创建触发器失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }
        Com<ITrigger> trg;
        hr = trs->Create(TASK_TRIGGER_LOGON, &trg.p);
        if (FAILED(hr) || !trg) {
            if (err) *err = L"创建登录触发器失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }
        trg->put_Enabled(VARIANT_TRUE);
        // 起始时间不能留空：留空时 schtasks /Query 显示 "Start Time: N/A"，
        // 触发器的生效边界不确定（老版本系统上会直接不触发）。给一个过去的固定值 =
        // 「从此刻起都有效」，登录触发本身不看它。
        trg->put_StartBoundary(Bstr(L"2000-01-01T00:00:00"));
        Com<ILogonTrigger> lt;
        if (SUCCEEDED(trg->QueryInterface(IID_ILogonTrigger, (void**)&lt.p)) && lt) {
            if (!user.empty()) lt->put_UserId(Bstr(user.c_str()));
            // 等 10 秒再起：登录瞬间 explorer 的托盘还没就绪，Shell_NotifyIcon 会失败——
            // 程序照样在跑，但用户看不到图标，等于「不生效」。
            lt->put_Delay(Bstr(L"PT10S"));
        }

        Com<IActionCollection> acts;
        hr = def->get_Actions(&acts.p);
        if (FAILED(hr) || !acts) {
            if (err) *err = L"创建动作集合失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }
        Com<IAction> act;
        hr = acts->Create(TASK_ACTION_EXEC, &act.p);
        if (FAILED(hr) || !act) {
            if (err) *err = L"创建启动动作失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }
        Com<IExecAction> ea;
        hr = act->QueryInterface(IID_IExecAction, (void**)&ea.p);
        if (FAILED(hr) || !ea) {
            if (err) *err = L"设置启动命令失败：" + HresultText(hr);
            if (owned) CoUninitialize();
            return false;
        }
        ea->put_Path(Bstr(exe.c_str()));
        size_t k = exe.find_last_of(L"\\/");
        if (k != std::wstring::npos) ea->put_WorkingDirectory(Bstr(exe.substr(0, k).c_str()));

        Com<IRegisteredTask> rt;
        // 任务 ACL 里显式给当前用户完全控制。默认 ACL 下，提权创建的任务在
        // 普通权限进程里连删除都做不到（实测 schtasks /Delete 报 Access denied），
        // 于是「关掉自启 / 降级成普通自启」会失败，留下一条登录时照样触发的幽灵任务。
        std::wstring sddl = L"D:(A;;FA;;;BA)(A;;FA;;;SY)";
        std::wstring sid = CurrentUserSid();
        if (!sid.empty()) sddl += L"(A;;FA;;;" + sid + L")";

        VARIANT noUser = VarEmpty();
        VARIANT noPwd = VarEmpty();
        VARIANT sd = VarBstr(sddl.c_str());
        hr = root->RegisterTaskDefinition(Bstr(kTaskName), def.p, TASK_CREATE_OR_UPDATE,
                                          noUser, noPwd, TASK_LOGON_INTERACTIVE_TOKEN, sd, &rt.p);
        VariantClear(&noUser);
        VariantClear(&noPwd);
        VariantClear(&sd);
        if (FAILED(hr)) {
            if (err) *err = L"登记计划任务失败：" + HresultText(hr);
        } else {
            ok = true;
        }
    }

    if (owned) CoUninitialize();
    return ok;
}

// 返回 false 表示任务还在（多半是没有管理员权限）。任务本来就不存在算成功。
static bool DeleteLogonTask(std::wstring* err) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        if (err) *err = L"COM 初始化失败：" + HresultText(hrCo);
        return false;
    }
    const bool owned = (hrCo == S_OK || hrCo == S_FALSE);
    bool ok = true;
    {
        Com<ITaskService> svc;
        Com<ITaskFolder> root;
        if (!OpenRoot(svc, root, err)) {
            ok = false;
        } else {
            HRESULT hr = root->DeleteTask(Bstr(kTaskName), 0);
            if (FAILED(hr) && HRESULT_CODE(hr) != ERROR_FILE_NOT_FOUND) {
                ok = false;
                if (err) *err = L"删除计划任务失败：" + HresultText(hr);
            }
        }
    }
    if (owned) CoUninitialize();
    return ok;
}

static bool WriteRunValue(const std::wstring& exe) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    std::wstring val = L"\"" + exe + L"\"";
    LONG rc = RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

// ================================================================== 状态查询
static void AppendLaunchLog(const std::wstring& line);

static std::wstring ReadRunValue() {
    HKEY k = nullptr;
    std::wstring out;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return out;
    wchar_t buf[1024]{};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(k, kRunValue, nullptr, &type, (BYTE*)buf, &cb) == ERROR_SUCCESS && type == REG_SZ)
        out.assign(buf);
    RegCloseKey(k);
    return out;
}

// 计划任务的完整 XML；任务不存在时返回空（GetTask 会报 FILE_NOT_FOUND，属正常分支）。
static std::wstring TaskXml() {
    std::wstring xml;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) return xml;
    const bool owned = (hrCo == S_OK || hrCo == S_FALSE);
    {
        Com<ITaskService> svc;
        Com<ITaskFolder> root;
        if (OpenRoot(svc, root, nullptr)) {
            Com<IRegisteredTask> rt;
            if (SUCCEEDED(root->GetTask(Bstr(kTaskName), &rt.p)) && rt) {
                BSTR b = nullptr;
                if (SUCCEEDED(rt->get_Xml(&b)) && b) {
                    xml.assign(b, SysStringLen(b));
                    SysFreeString(b);
                }
            }
        }
    }
    if (owned) CoUninitialize();
    return xml;
}

// 期望形式：管理员模式要的是「登录即以最高权限启动」，只有计划任务能做到；
// 但标准用户（不在管理员组）提不了权、也建不了 HighestAvailable 任务，
// 这时必须退回 Run 项——否则开关点下去看着成功了，实际什么都没装上。
static bool WantTaskForm() { return g_cfg.admin && IsUserInAdminGroup(); }

static bool RunFormOk(const std::wstring& exe) {
    if (ReadRunValue() != (L"\"" + exe + L"\"")) return false;
    return TaskXml().empty();          // 两种实现不能同时存在，否则登录会起两个实例
}

static bool TaskFormOk(const std::wstring& exe) {
    std::wstring xml = TaskXml();
    if (xml.empty()) return false;
    if (xml.find(exe) == std::wstring::npos) return false;                          // exe 被挪过位置
    if (xml.find(L"HighestAvailable") == std::wstring::npos) return false;          // 权限级别不对
    if (xml.find(L"<DisallowStartIfOnBatteries>true<") != std::wstring::npos) return false;
    return ReadRunValue().empty();
}

// ================================================================== 对外
bool SetAutostart(bool on, std::wstring* err) {
    if (err) err->clear();
    std::wstring exe = ExePath();
    if (exe.empty()) {
        if (err) *err = L"取不到程序自身路径";
        return false;
    }

    // 先清掉两种实现，避免「Run 项 + 计划任务」同时存在、登录时起两个实例。
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunValue);
        RegCloseKey(k);
    }
    std::wstring delErr;
    const bool taskGone = DeleteLogonTask(&delErr);

    if (!on) {
        if (!taskGone) {
            if (err) *err = delErr;
            return false;
        }
        return true;
    }

    if (WantTaskForm()) {
        if (!IsProcessElevated()) {
            // 建「最高权限」的计划任务需要管理员令牌。这里先落 Run 项占位，
            // 马上要发生的提权重启会让新实例对账后把形式换过来（见 ReconcileAutostart）。
            if (!WriteRunValue(exe)) {
                if (err) *err = L"写入注册表 Run 项失败（HKCU\\...\\Run）";
                return false;
            }
            if (err) *err = L"计划任务需要管理员权限，已先设为普通自启";
            return true;
        }
        std::wstring e;
        if (CreateLogonTask(exe, &e)) return true;
        // 建任务失败（组策略禁用 / 服务被停）就退回 Run 项：
        // 代价是登录时可能弹一次 UAC，但「开机自启」至少还是生效的。
        if (!WriteRunValue(exe)) {
            if (err) *err = e.empty() ? L"写入注册表 Run 项失败" : e;
            return false;
        }
        if (err) *err = (e.empty() ? L"计划任务创建失败" : e) + std::wstring(L"；已退回普通权限自启");
        return true;
    }

    if (!WriteRunValue(exe)) {
        if (err) *err = L"写入注册表 Run 项失败（HKCU\\...\\Run）";
        return false;
    }
    if (!taskGone && err) *err = delErr + std::wstring(L"；登录时可能重复启动");
    return true;
}

// 启动时把「实际的登记形式」与「应有形式」对齐。这一步不能省：
//   · 用户先开自启（普通 → Run 项），再开管理员模式 → 必须换成计划任务，
//     否则下次登录仍然是普通权限，Win+V 在高权限窗口里照样失灵；
//   · 反过来关掉管理员模式，得把计划任务降回 Run 项，否则登录时会莫名弹 UAC；
//   · exe 换过位置（重新构建、挪目录）时旧的登记指向不存在的路径，自启等于失效。
// 只在真的不一致时才重写，避免每次启动都动注册表/任务。
void ReconcileAutostart() {
    std::wstring exe = ExePath();
    if (exe.empty()) return;
    if (!g_cfg.autostart) {
        if (ReadRunValue().empty() && TaskXml().empty()) return;
        std::wstring e;
        if (!SetAutostart(false, &e)) AppendLaunchLog(L"autostart(off): " + e);
        return;
    }
    if (WantTaskForm() ? TaskFormOk(exe) : RunFormOk(exe)) return;
    std::wstring e;
    if (!SetAutostart(true, &e) || !e.empty())
        AppendLaunchLog(L"autostart(on): " + (e.empty() ? std::wstring(L"写入失败") : e));
}

// ================================================================== 启动留痕
// 「自启没生效」这类反馈没法靠猜：开机时到底有没有被拉起、起来的是什么权限、
// 自启登记有没有对账成功，写一行日志就能确定。只保留最近几十行，不会长成垃圾。
static void AppendLaunchLog(const std::wstring& line) {
    std::wstring path = ConfigDir() + L"\\launch.log";

    std::string old;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while (old.size() < 8192 && (n = fread(buf, 1, sizeof(buf), f)) > 0) old.append(buf, n);
        fclose(f);
    }
    // 只留最后 40 行
    int lines = 0;
    for (char ch : old) if (ch == '\n') ++lines;
    while (lines >= 40) {
        size_t p = old.find('\n');
        if (p == std::string::npos) { old.clear(); break; }
        old.erase(0, p + 1);
        --lines;
    }
    old += W2U8(line);

    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        fwrite(old.data(), 1, old.size(), f);
        fclose(f);
    }
}

// 父进程 pid 与可执行文件名。只看 argv 分不清「开机自启拉起的」和「用户双击的」
// ——两者的命令行可能一字不差，父进程才是证据：
//   explorer.exe      = 双击 / Run 项（explorer 拉起）
//   svchost.exe       = 计划任务（任务计划服务拉起）
//   powershell.exe    = 命令行/脚本测试
//   本项目 exe 本身   = 上次运行的自己做了提权重启
static void ParentInfo(DWORD* pid, std::wstring* path) {
    *pid = 0;
    path->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) { *pid = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!*pid) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
    if (!h) return;
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) *path = buf;
    CloseHandle(h);
}

void LogEvent(const std::wstring& line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    std::swprintf(head, 64, L"%04d-%02d-%02d %02d:%02d:%02d pid=%lu ", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond,
                  (unsigned long)GetCurrentProcessId());
    AppendLaunchLog(std::wstring(head) + line + L"\r\n");
}

void LogLaunch() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD ppid = 0;
    std::wstring ppath;
    ParentInfo(&ppid, &ppath);
    // 父进程路径只留文件名：整条路径会把这一行撑得很长，而排查只需要认人。
    size_t slash = ppath.find_last_of(L'\\');
    std::wstring pname = (slash == std::wstring::npos) ? ppath : ppath.substr(slash + 1);
    wchar_t head[2048];
    std::swprintf(head, 2048,
                  L"%04d-%02d-%02d %02d:%02d:%02d pid=%lu elevated=%d ppid=%lu parent=\"%ls\" "
                  L"argv=\"%ls\"\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  (unsigned long)GetCurrentProcessId(), IsProcessElevated() ? 1 : 0,
                  (unsigned long)ppid, pname.c_str(), GetCommandLineW());
    AppendLaunchLog(head);
}
