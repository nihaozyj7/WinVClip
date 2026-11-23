// path.h —— D2D 几何路径的小包装。
//
// 图标是「一堆线段/圆弧/贝塞尔」描出来的，直接用 GeometrySink 写太啰嗦，
// 这里把它包成 Move/Line/Circle/RoundRect 几个动词，DrawIcon 就只剩坐标。
//
// 生命周期：构造时向 D2D 申请几何，析构时释放；不可拷贝（持有 COM 指针）。
// 用法是先 Move 再 Line/Bezier，最后 EndOpen/EndClosed 收一条子路径，
// 多条子路径在同一个 geo 里靠填充模式（WINDING）实现镂空。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include "core/app_state.h"   // g_d2d
#include "render/dc.h"        // Brush

struct PathB {
    PathB(const PathB&) = delete;
    PathB& operator=(const PathB&) = delete;

    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    bool used = false;

    PathB() {
        if (g_d2d && SUCCEEDED(g_d2d->CreatePathGeometry(&geo)) && geo && SUCCEEDED(geo->Open(&sink)))
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);   // 让反向绕序的子路径成为镂空
    }
    ~PathB() {
        if (sink) sink->Release();
        if (geo) geo->Release();
    }
    bool ok() const { return geo && sink; }

    void Move(float x, float y) {
        if (!sink) return;
        sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        used = true;
    }

    // 填充用的起点。BeginFigure 的 filled/hollow 决定该子路径参不参与填充：
    // 图标是描边的，用 Move（hollow）即可；一旦要 FillGeometry（按钮高亮块、
    // 页签底色），figure 必须开成 filled，否则路径明明存在却什么都填不出来。
    void MoveFill(float x, float y) {
        if (!sink) return;
        sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        used = true;
    }
    void Line(float x, float y) { if (sink) sink->AddLine(D2D1::Point2F(x, y)); }
    void EndOpen()   { if (sink) sink->EndFigure(D2D1_FIGURE_END_OPEN); }
    void EndClosed() { if (sink) sink->EndFigure(D2D1_FIGURE_END_CLOSED); }

    // 四段三次贝塞尔近似整圆（比 ArcSegment 更稳，不受角度限制）
    void Circle(float cx, float cy, float r, bool cw = true) {
        if (!sink) return;
        const float k = 0.5522847498f * r;
        if (cw) {
            Move(cx, cy - r);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + k, cy - r),
                                                D2D1::Point2F(cx + r, cy - k),
                                                D2D1::Point2F(cx + r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy + k),
                                                D2D1::Point2F(cx + k, cy + r),
                                                D2D1::Point2F(cx, cy + r)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - k, cy + r),
                                                D2D1::Point2F(cx - r, cy + k),
                                                D2D1::Point2F(cx - r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy - k),
                                                D2D1::Point2F(cx - k, cy - r),
                                                D2D1::Point2F(cx, cy - r)));
        } else {
            Move(cx, cy - r);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - k, cy - r),
                                                D2D1::Point2F(cx - r, cy - k),
                                                D2D1::Point2F(cx - r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy + k),
                                                D2D1::Point2F(cx - k, cy + r),
                                                D2D1::Point2F(cx, cy + r)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + k, cy + r),
                                                D2D1::Point2F(cx + r, cy + k),
                                                D2D1::Point2F(cx + r, cy)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy - k),
                                                D2D1::Point2F(cx + k, cy - r),
                                                D2D1::Point2F(cx, cy - r)));
        }
        EndClosed();
    }

    void RoundRect(float l, float t, float rr, float b, float rad) {
        if (!sink) return;
        float r = rad;
        if (r > (rr - l) / 2.0f) r = (rr - l) / 2.0f;
        if (r > (b - t) / 2.0f) r = (b - t) / 2.0f;
        if (r < 0.1f) {
            Move(l, t); Line(rr, t); Line(rr, b); Line(l, b); EndClosed();
            return;
        }
        const float k = 0.5522847498f * r;
        Move(l + r, t);
        Line(rr - r, t);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr - r + k, t),
                                            D2D1::Point2F(rr, t + r - k),
                                            D2D1::Point2F(rr, t + r)));
        Line(rr, b - r);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rr, b - r + k),
                                            D2D1::Point2F(rr - r + k, b),
                                            D2D1::Point2F(rr - r, b)));
        Line(l + r, b);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(l + r - k, b),
                                            D2D1::Point2F(l, b - r + k),
                                            D2D1::Point2F(l, b - r)));
        Line(l, t + r);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(l, t + r - k),
                                            D2D1::Point2F(l + r - k, t),
                                            D2D1::Point2F(l + r, t)));
        EndClosed();
    }

    void Stroke(COLORREF c, float width) {
        if (!ok() || !used) return;
        sink->Close();
        sink->Release();
        sink = nullptr;
        Brush(c);
        g_rt->DrawGeometry(geo, g_brush, width < 1.0f ? 1.0f : width, g_roundStroke);
    }

    void Fill(COLORREF c) {
        if (!ok() || !used) return;
        sink->Close();
        sink->Release();
        sink = nullptr;
        Brush(c);
        g_rt->FillGeometry(geo, g_brush);
    }
};

// ------------------------------------------------------------------ 图标