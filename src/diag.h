// 诊断：性能探针与日志。默认全部关闭，关闭时开销只有一次环境变量读取。
//
// 这里只管「把数字/字符串落到 %TEMP% 下的文件」，不解释业务语义：
//   · WINVCLIP_PERF=1 或 --perf  → 绘制各阶段耗时，每 30 帧一行 winvclip_perf.log
//   · WV_TRACE=1                 → 焦点/前台排查日志 winvclip-trace.txt
//
// 语义化的汇总（PerfFlush 要把当前页、表情命中率一起写出来）留在 main，
// 因为那需要读取业务状态；本模块保持「零业务依赖」，谁都可以调用。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wv {

struct PerfAcc {
    LONGLONG begin = 0, prep = 0, head = 0, body = 0, foot = 0, end = 0, blit = 0;
    LONGLONG total = 0, maxTotal = 0;
    int n = 0;
};

// 性能探针的开关与累加器。绘制路径在高频循环里直接读写，故按全局暴露，
// 不做 getter：每帧每阶段一次函数调用在这里是可测的开销。
extern bool     g_perfOn;
extern PerfAcc  g_perfAcc;
extern LARGE_INTEGER g_perfFreq;

LONGLONG PerfNow();
double   PerfMs(LONGLONG t);

// 追加一行到 %TEMP%\winvclip_perf.log（无论探针是否开启都写，供预热统计等使用）
void PerfAppend(const char* line);

// 解析命令行/环境变量，决定是否开启探针。必须在主循环前调用一次。
void PerfInit();

// 便捷包装：snprintf 两个 int 后交给 PerfAppend，用于字形/表情预热统计
void WarmLog(const char* fmt, int a, int b);

// 诊断日志（WV_TRACE=1 时写 %TEMP%\winvclip-trace.txt）
void Trace(const char* fmt, ...);

}  // namespace wv
