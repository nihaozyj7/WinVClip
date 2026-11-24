// vmmap.c —— 遍历目标进程地址空间，按类型统计已提交内存并列出最大的若干区域。
// 用途：定位 winvclip 这类 GUI 程序的常驻内存构成（私有堆 / 映射文件 / 模块映像）。
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { ULONGLONG size; ULONGLONG base; DWORD type; DWORD prot; } Region;

static int cmp_desc(const void* a, const void* b) {
    const Region* x = (const Region*)a; const Region* y = (const Region*)b;
    if (x->size < y->size) return 1;
    if (x->size > y->size) return -1;
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: vmmap <pid>\n"); return 1; }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 1; }

    size_t cap = 1 << 16, n = 0;
    Region* rg = (Region*)malloc(cap * sizeof(Region));
    ULONGLONG tot[3] = {0, 0, 0};
    unsigned char* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        ULONGLONG sz = (ULONGLONG)mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            int t = mbi.Type == MEM_PRIVATE ? 0 : (mbi.Type == MEM_IMAGE ? 1 : 2);
            tot[t] += sz;
            if (n < cap) { rg[n].size = sz; rg[n].base = (ULONGLONG)mbi.BaseAddress;
                           rg[n].type = mbi.Type; rg[n].prot = mbi.Protect; ++n; }
        }
        ULONGLONG next = (ULONGLONG)mbi.BaseAddress + sz;
        if (next <= (ULONGLONG)addr) break;
        addr = (unsigned char*)next;
    }
    CloseHandle(h);
    wprintf(L"committed: private=%.1fMB image=%.1fMB mapped=%.1fMB sum=%.1fMB\n",
            tot[0] / 1048576.0, tot[1] / 1048576.0, tot[2] / 1048576.0,
            (tot[0] + tot[1] + tot[2]) / 1048576.0);
    qsort(rg, n, sizeof(Region), cmp_desc);
    wprintf(L"--- biggest 25 committed regions ---\n");
    for (size_t i = 0; i < n && i < 25; ++i) {
        const wchar_t* tn = rg[i].type == MEM_PRIVATE ? L"PRIVATE" :
                            rg[i].type == MEM_IMAGE ? L"IMAGE" : L"MAPPED";
        wprintf(L"%p  %8.2f MB  %-7s prot=0x%02lX\n", (void*)rg[i].base,
                rg[i].size / 1048576.0, tn, (unsigned long)rg[i].prot);
    }
    free(rg);
    return 0;
}
