// icons.cpp —— 实现见 icons.h。
//
// 坐标系约定：所有 case 里的数字都按 24×24 设计稿写，由 X()/Y() 折算到
// 实际 box。新增图标时不要自己算像素，跟着写 24 基准的数字即可。
#include "render/icons.h"

#include <cmath>

#include "core/app_state.h"   // g_rt / g_painting
#include "core/util.h"        // S
#include "render/path.h"      // PathB

// 在 box 内按 24×24 坐标系描边绘制线性图标，stroke 为线宽
// 在 box 内按 24×24 坐标系绘制线性图标，stroke 为 24 坐标系下的线宽
void DrawIcon(HDC dc, IconId id, RECT box, COLORREF c, float stroke) {
    (void)dc;
    if (!g_rt || !g_painting) return;
    float w = (float)(box.right - box.left), h = (float)(box.bottom - box.top);
    float sz = w < h ? w : h;
    float k = sz / 24.0f;
    float ox = (float)box.left + (w - sz) / 2.0f;
    float oy = (float)box.top + (h - sz) / 2.0f;
    auto X = [&](double v) { return ox + (float)(v * k); };
    auto Y = [&](double v) { return oy + (float)(v * k); };
    float px = stroke * k;               // 线宽按尺寸折算，不取整

    PathB s, f;                          // s = 描边路径，f = 填充路径
    switch (id) {
    case IC_SEARCH:
        s.Circle(X(11), Y(11), 7 * k);
        s.Move(X(20), Y(20)); s.Line(X(16.2), Y(16.2)); s.EndOpen();
        break;
    case IC_GEAR: {
        // 线性齿轮：外圈 + 8 根短齿 + 中心轴孔（与其它图标同风格，走描边路径）
        s.Circle(X(12), Y(12), 6.2 * k);
        s.Circle(X(12), Y(12), 2.5 * k);
        for (int i = 0; i < 8; ++i) {
            double a = i * 3.14159265 / 4.0;
            s.Move(X(12 + 6.4 * cos(a)), Y(12 + 6.4 * sin(a)));
            s.Line(X(12 + 9.3 * cos(a)), Y(12 + 9.3 * sin(a)));
            s.EndOpen();
        }
        break; }
    case IC_X:
        s.Move(X(6), Y(6));  s.Line(X(18), Y(18)); s.EndOpen();
        s.Move(X(18), Y(6)); s.Line(X(6), Y(18));  s.EndOpen();
        break;
    case IC_CLIP:
        s.RoundRect(X(5), Y(4), X(19), Y(22), 2 * k);
        s.RoundRect(X(8.5), Y(2), X(15.5), Y(6), 1 * k);
        break;
    case IC_TEXT:
        s.Move(X(4), Y(6));  s.Line(X(20), Y(6));  s.EndOpen();
        s.Move(X(4), Y(12)); s.Line(X(15), Y(12)); s.EndOpen();
        s.Move(X(4), Y(18)); s.Line(X(11), Y(18)); s.EndOpen();
        break;
    case IC_CODE:
        s.Move(X(9), Y(6));  s.Line(X(3), Y(12));  s.Line(X(9), Y(18));  s.EndOpen();
        s.Move(X(15), Y(6)); s.Line(X(21), Y(12)); s.Line(X(15), Y(18)); s.EndOpen();
        break;
    case IC_IMAGE:
        s.RoundRect(X(3), Y(3), X(21), Y(21), 2 * k);
        s.Circle(X(9), Y(9), 2 * k);
        s.Move(X(20), Y(14)); s.Line(X(15), Y(9)); s.Line(X(4), Y(20)); s.EndOpen();
        break;
    case IC_FILE:
        s.Move(X(14), Y(2));
        s.Line(X(6), Y(2));  s.Line(X(4), Y(4));  s.Line(X(4), Y(20));
        s.Line(X(6), Y(22)); s.Line(X(18), Y(22)); s.Line(X(20), Y(20));
        s.Line(X(20), Y(8)); s.Line(X(14), Y(2));
        s.EndOpen();
        s.Move(X(14), Y(2)); s.Line(X(14), Y(8)); s.Line(X(20), Y(8)); s.EndOpen();
        break;
    case IC_LINK:
        s.RoundRect(X(2), Y(9), X(13), Y(15), 3 * k);
        s.RoundRect(X(11), Y(9), X(22), Y(15), 3 * k);
        break;
    case IC_PIN:
        s.Move(X(12), Y(16)); s.Line(X(12), Y(22)); s.EndOpen();
        s.Move(X(7), Y(11));  s.Line(X(17), Y(11)); s.EndOpen();
        s.Move(X(9.5), Y(2));
        s.Line(X(14.5), Y(2)); s.Line(X(15), Y(6)); s.Line(X(15), Y(11));
        s.EndOpen();
        s.Move(X(9), Y(11));  s.Line(X(9), Y(6)); s.Line(X(9.5), Y(2)); s.EndOpen();
        break;
    case IC_STAR: {
        double a0 = -3.14159265 / 2;
        s.Move(X(12 + 9.2 * cos(a0)), Y(12 + 9.2 * sin(a0)));
        for (int i = 1; i < 10; ++i) {
            double a = a0 + i * 3.14159265 / 5;
            double rr = (i % 2 == 0) ? 9.2 : 3.9;
            s.Line(X(12 + rr * cos(a)), Y(12 + rr * sin(a)));
        }
        s.EndClosed();
        break; }
    case IC_MORE:
        for (int i = 0; i < 3; ++i)
            s.Circle(X(12), Y(5 + i * 7), 0.55 * k);
        break;
    case IC_TRASH:
        s.Move(X(3), Y(6));  s.Line(X(21), Y(6)); s.EndOpen();
        s.Move(X(8), Y(6));  s.Line(X(8), Y(3)); s.Line(X(16), Y(3)); s.Line(X(16), Y(6));
        s.EndOpen();
        s.Move(X(19), Y(6)); s.Line(X(18), Y(21)); s.Line(X(6), Y(21)); s.Line(X(5), Y(6));
        s.EndOpen();
        break;
    case IC_CHECK:
        s.Move(X(20), Y(6)); s.Line(X(9), Y(17)); s.Line(X(4), Y(12)); s.EndOpen();
        break;
    case IC_COPY:
        s.RoundRect(X(9), Y(9), X(21), Y(21), 1.5 * k);
        s.Move(X(5), Y(15));
        s.Line(X(4), Y(15)); s.Line(X(3), Y(13)); s.Line(X(3), Y(4));
        s.Line(X(5), Y(3));  s.Line(X(14), Y(3)); s.Line(X(15), Y(4));
        s.Line(X(15), Y(5));
        s.EndOpen();
        break;
    case IC_SAVE:
        // 下载式「另存为」：向下的箭头落进托盘
        s.Move(X(12), Y(3));  s.Line(X(12), Y(15)); s.EndOpen();
        s.Move(X(7.5), Y(10.5)); s.Line(X(12), Y(15)); s.Line(X(16.5), Y(10.5)); s.EndOpen();
        s.Move(X(4), Y(17.5)); s.Line(X(4), Y(20)); s.Line(X(20), Y(20));
        s.Line(X(20), Y(17.5)); s.EndOpen();
        break;
    }

    f.Fill(c);
    s.Stroke(c, px);
}

IconId ItemIcon(const ClipItem& it) {
    if (it.kind == 2) return IC_IMAGE;
    if (it.kind == 1) return IC_FILE;
    if (it.isLink) return IC_LINK;
    if (it.isCode) return IC_CODE;
    return IC_TEXT;
}