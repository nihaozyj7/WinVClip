// UIA 插入符诊断：把焦点元素的各种「取矩形」通道全打印出来。
// 只读，不改任何东西。用法：uia_diag.exe [等待秒数]
// 编译：g++ -std=c++17 -O2 tools/uia_diag.cpp -o tools/uia_diag.exe -luiautomationcore -lole32 -loleaut32
#include <windows.h>
#include <initguid.h>
#include <uiautomation.h>
#include <stdio.h>
#include <math.h>

static void DumpRects(const char* tag, IUIAutomationTextRange* r) {
    SAFEARRAY* sa = nullptr;
    HRESULT hr = r->GetBoundingRectangles(&sa);
    printf("    %s: hr=%08lx", tag, (unsigned long)hr);
    if (FAILED(hr) || !sa) { printf(" (no array)\n"); return; }
    long lo = 0, hi = -1, n = 0;
    SafeArrayGetLBound(sa, 1, &lo);
    SafeArrayGetUBound(sa, 1, &hi);
    if (hi >= lo) n = (hi - lo + 1) / 4;
    printf(" n=%ld:", n);
    double* d = nullptr;
    if (SUCCEEDED(SafeArrayAccessData(sa, (void**)&d)) && d) {
        for (long i = 0; i < n && i < 12; i++)
            printf(" [%d,%d %dx%d]", (int)lround(d[i*4]), (int)lround(d[i*4+1]),
                   (int)lround(d[i*4+2]), (int)lround(d[i*4+3]));
        SafeArrayUnaccessData(sa);
    }
    SafeArrayDestroy(sa);
    printf("\n");
}

static void DumpText(const char* tag, IUIAutomationTextRange* r) {
    BSTR s = nullptr;
    if (SUCCEEDED(r->GetText(80, &s)) && s) {
        printf("    %s text='%ls'\n", tag, s);
        SysFreeString(s);
    }
}

int main(int argc, char** argv) {
    // 可选参数：目标窗口句柄（十六进制或十进制）。给了就先把它置前，
    // 否则焦点元素可能属于别的窗口（比如刚被工具抢走前台的宿主窗口）。
    if (argc > 1) {
        HWND h = (HWND)(ULONG_PTR)strtoull(argv[1], nullptr, 0);
        if (h && IsWindow(h)) {
            SetForegroundWindow(h);
            Sleep(400);
            printf("focused target %p\n", (void*)h);
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IUIAutomation* uia = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&uia);
    if (FAILED(hr) || !uia) { printf("create fail %08lx\n", (unsigned long)hr); return 1; }
    IUIAutomation2* uia2 = nullptr;
    if (SUCCEEDED(uia->QueryInterface(IID_IUIAutomation2, (void**)&uia2)) && uia2) {
        uia2->put_ConnectionTimeout(2000);
        uia2->Release();
    }

    HWND fg = GetForegroundWindow();
    printf("fg hwnd=%p\n", (void*)fg);

    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->GetFocusedElement(&el)) || !el) { printf("no focused element\n"); return 1; }
    int pid = 0; CONTROLTYPEID ct = 0;
    el->get_CurrentProcessId(&pid);
    el->get_CurrentControlType(&ct);
    BSTR nm = nullptr; el->get_CurrentName(&nm);
    BSTR cls = nullptr; el->get_CurrentClassName(&cls);
    RECT box{};
    el->get_CurrentBoundingRectangle(&box);
    printf("el pid=%d ct=%d class=%ls name='%ls'\n", pid, (int)ct,
           cls ? cls : L"", nm ? nm : L"");
    printf("  el box=[%ld,%ld %ldx%ld]\n", box.left, box.top,
           box.right - box.left, box.bottom - box.top);
    if (nm) SysFreeString(nm);
    if (cls) SysFreeString(cls);

    // 1) TextPattern2::GetCaretRange
    IUIAutomationTextPattern2* tp2 = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                          (void**)&tp2)) && tp2) {
        BOOL active = FALSE;
        IUIAutomationTextRange* r = nullptr;
        HRESULT h2 = tp2->GetCaretRange(&active, &r);
        printf("  caretRange: hr=%08lx active=%d range=%p\n", (unsigned long)h2, (int)active, (void*)r);
        if (r) {
            DumpText("caret", r);
            DumpRects("caret", r);
            // 退化范围扩展到一个字符后再取
            IUIAutomationTextRange* c = nullptr;
            if (SUCCEEDED(r->Clone(&c)) && c) {
                c->ExpandToEnclosingUnit(TextUnit_Character);
                DumpText("caret+expandChar", c);
                DumpRects("caret+expandChar", c);
                c->Release();
            }
            r->Release();
        }
        tp2->Release();
    } else {
        printf("  caretRange: no TextPattern2\n");
    }

    // 2) TextPattern::GetSelection
    IUIAutomationTextPattern* tp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                          (void**)&tp)) && tp) {
        IUIAutomationTextRangeArray* arr = nullptr;
        HRESULT h3 = tp->GetSelection(&arr);
        printf("  selection: hr=%08lx\n", (unsigned long)h3);
        if (arr) {
            int n = 0; arr->get_Length(&n);
            printf("  selection count=%d\n", n);
            for (int i = 0; i < n; i++) {
                IUIAutomationTextRange* r = nullptr;
                if (FAILED(arr->GetElement(i, &r)) || !r) continue;
                printf("  -- sel[%d]\n", i);
                DumpText("sel", r);
                DumpRects("sel", r);
                // 退化范围（start==end）时试试两种「造出非空范围」的办法
                IUIAutomationTextRange* c = nullptr;
                if (SUCCEEDED(r->Clone(&c)) && c) {
                    c->ExpandToEnclosingUnit(TextUnit_Character);
                    DumpText("sel+expandChar", c);
                    DumpRects("sel+expandChar", c);
                    c->Release();
                }
                IUIAutomationTextRange* d = nullptr;
                if (SUCCEEDED(r->Clone(&d)) && d) {
                    // 把起点往前挪一个字符：范围覆盖插入符左边那个字符
                    int moved = 0;
                    d->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved);
                    printf("    sel+startBack moved=%d\n", moved);
                    DumpText("sel+startBack", d);
                    DumpRects("sel+startBack", d);
                    d->Release();
                }
                r->Release();
            }
            arr->Release();
        }
        // RangeFromPoint：按几何点取文本范围，rect 由布局算出，可能比退化选区可靠
        {
            POINT pts[2] = {{(box.left + box.right) / 2, (box.top + (box.bottom - box.top) / 4)},
                            {(box.left + 20), box.top + 12}};
            for (int k = 0; k < 2; k++) {
                IUIAutomationTextRange* r = nullptr;
                HRESULT hp = tp->RangeFromPoint(pts[k], &r);
                printf("  rangeFromPoint(%ld,%ld): hr=%08lx\n", pts[k].x, pts[k].y, (unsigned long)hp);
                if (r) {
                    DumpText("rfp", r);
                    DumpRects("rfp", r);
                    IUIAutomationTextRange* c = nullptr;
                    if (SUCCEEDED(r->Clone(&c)) && c) {
                        c->ExpandToEnclosingUnit(TextUnit_Character);
                        DumpRects("rfp+expandChar", c);
                        c->Release();
                    }
                    r->Release();
                }
            }
        }
        tp->Release();
    } else {
        printf("  selection: no TextPattern\n");
    }

    // 3) 其它 pattern 可用性
    {
        struct { const char* name; PATTERNID id; } pats[] = {
            {"TextPattern", UIA_TextPatternId}, {"TextPattern2", UIA_TextPattern2Id},
            {"Value", UIA_ValuePatternId}, {"LegacyIAccessible", UIA_LegacyIAccessiblePatternId},
            {"TextChild", UIA_TextChildPatternId}, {"Annotation", UIA_AnnotationPatternId},
        };
        for (auto& p : pats) {
            IUnknown* unk = nullptr;
            VARIANT v; VariantInit(&v);
            el->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId + (p.id - UIA_TextPatternId), &v);
            printf("  pattern %s: avail=%d", p.name, (v.vt == VT_BOOL) ? (int)v.boolVal : -1);
            HRESULT hr2 = el->GetCurrentPattern(p.id, &unk);
            printf(" hr=%08lx\n", (unsigned long)hr2);
            if (unk) unk->Release();
        }
    }

    el->Release();
    uia->Release();
    CoUninitialize();
    return 0;
}
