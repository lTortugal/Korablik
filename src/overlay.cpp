// Оверлей выделения области: «замороженный» затемнённый снимок, мышь, маркеры,
// компактная панель из трёх действий, горячие клавиши 1/2/3/Enter/Esc
#include "app.h"
#include <algorithm>
#include <cstdlib>

static const wchar_t* CLS_OVERLAY = L"WinShotOverlayCls";
static const wchar_t* CLS_TOOLBAR = L"WinShotToolbarCls";

// внешний из main.cpp — скрытое окно приложения (владелец оверлея)
extern HWND g_hwndMain;
extern bool g_quitRequested;

namespace {

struct Overlay {
    HWND hwnd = nullptr;
    HWND toolbar = nullptr;
    bool done = false;

    int sw = 0, sh = 0; // физический размер монитора под курсором
    int ox = 0, oy = 0; // начало монитора в экранных координатах
    UINT dpi = 96;

    // поверхности: shotDC — яркий оригинал, dimDC — затемнённый фон, memDC — двойная буферизация
    HDC shotDC = nullptr, dimDC = nullptr, memDC = nullptr;
    HBITMAP shotBmp = nullptr, dimBmp = nullptr, memBmp = nullptr;
    RGBQUAD* shotBits = nullptr; // пиксели shotBmp (берём крой результата отсюда)

    HFONT font = nullptr;
    HFONT fontHint = nullptr;

    bool hasSel = false;
    RECT sel = {};   // нормализованный, в координатах оверлея
    POINT anchor = {};

    int dragMode = 0; // 0 нет, 1 новое выделение, 2 перемещение, 3..10 маркер (3+индекс), 5 кисть
    RECT selStart = {};
    POINT lastPos = {};
    bool captured = false;
    int hoverHandle = -99;

    // рисование поверх выделения (как в Lightshot)
    bool penMode = false;
    struct Stroke { std::vector<POINT> pts; };
    std::vector<Stroke> strokes;

    // панель
    RECT tbBtns[4] = {};
    int tbW = 0, tbH = 0;
    int hoverBtn = -1;
    bool tracking = false;

    int hs = 14; // допуск попадания в маркер, физ. px
};

Overlay* ov = nullptr;
bool g_overlayOpen = false;

int sc(Overlay* o, int logical) { return MulDiv(logical, (int)o->dpi, 96); }

RECT norm_rect(const RECT& r) {
    RECT n;
    n.left = std::min(r.left, r.right);
    n.right = std::max(r.left, r.right);
    n.top = std::min(r.top, r.bottom);
    n.bottom = std::max(r.top, r.bottom);
    return n;
}

// 8 маркеров: 0 TL,1 T,2 TR,3 R,4 BR,5 B,6 BL,7 L; -2 — внутри, -1 — мимо
POINT handle_center(const RECT& s, int i) {
    int cx = (s.left + s.right) / 2, cy = (s.top + s.bottom) / 2;
    switch (i) {
        case 0: return {s.left, s.top};
        case 1: return {cx, s.top};
        case 2: return {s.right, s.top};
        case 3: return {s.right, cy};
        case 4: return {s.right, s.bottom};
        case 5: return {cx, s.bottom};
        case 6: return {s.left, s.bottom};
        default: return {s.left, cy};
    }
}

int hit_test(const RECT& s, int hs, POINT p) {
    for (int i = 0; i < 8; i++) {
        POINT c = handle_center(s, i);
        if (abs(p.x - c.x) <= hs && abs(p.y - c.y) <= hs) return i;
    }
    if (p.x >= s.left && p.x <= s.right && p.y >= s.top && p.y <= s.bottom) return -2;
    return -1;
}

HCURSOR cursor_for_hit(int hit) {
    switch (hit) {
        case 0: case 4: return LoadCursor(nullptr, IDC_SIZENWSE);
        case 2: case 6: return LoadCursor(nullptr, IDC_SIZENESW);
        case 1: case 5: return LoadCursor(nullptr, IDC_SIZENS);
        case 3: case 7: return LoadCursor(nullptr, IDC_SIZEWE);
        case -2: return LoadCursor(nullptr, IDC_SIZEALL);
        default: return LoadCursor(nullptr, IDC_ARROW);
    }
}

void clamp_rect(RECT& r, int sw, int sh) {
    if (r.left < 0) { r.right -= r.left; r.left = 0; }
    if (r.top < 0) { r.bottom -= r.top; r.top = 0; }
    if (r.right > sw) { r.left -= (r.right - sw); r.right = sw; }
    if (r.bottom > sh) { r.top -= (r.bottom - sh); r.bottom = sh; }
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
}

void update_sel_from_drag(Overlay* o, POINT p) {
    int x = std::max(0, std::min((int)p.x, o->sw));
    int y = std::max(0, std::min((int)p.y, o->sh));
    if (o->dragMode == 1) {
        RECT r = {o->anchor.x, o->anchor.y, x, y};
        o->sel = norm_rect(r);
        o->hasSel = (o->sel.right - o->sel.left >= 3) && (o->sel.bottom - o->sel.top >= 3);
    } else if (o->dragMode == 2) {
        int dx = x - o->lastPos.x, dy = y - o->lastPos.y;
        RECT r = o->sel;
        r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
        clamp_rect(r, o->sw, o->sh);
        o->sel = r;
    } else if (o->dragMode >= 3) {
        int i = o->dragMode - 3;
        RECT r = o->selStart;
        switch (i) {
            case 0: r.left = x; r.top = y; break;
            case 1: r.top = y; break;
            case 2: r.right = x; r.top = y; break;
            case 3: r.right = x; break;
            case 4: r.right = x; r.bottom = y; break;
            case 5: r.bottom = y; break;
            case 6: r.left = x; r.bottom = y; break;
            case 7: r.left = x; break;
        }
        o->sel = norm_rect(r);
        o->hasSel = (o->sel.right - o->sel.left >= 3) && (o->sel.bottom - o->sel.top >= 3);
    }
    o->lastPos = {x, y};
}

void hide_toolbar(Overlay* o) {
    if (o->toolbar) ShowWindow(o->toolbar, SW_HIDE);
    o->hoverBtn = -1;
}

void show_toolbar(Overlay* o) {
    if (!o->toolbar || !o->hasSel) return;
    RECT s = o->sel;
    int g = sc(o, 8);
    int x = s.right - o->tbW + g;
    int y = s.bottom + g;
    if (y + o->tbH > o->sh) y = s.top - o->tbH - g;
    if (y < sc(o, 4)) y = o->sh - o->tbH - sc(o, 4);
    if (x < sc(o, 4)) x = sc(o, 4);
    if (x + o->tbW > o->sw) x = o->sw - o->tbW - sc(o, 4);
    // панель — отдельное top-level окно, координаты экраные: прибавляем начало монитора
    SetWindowPos(o->toolbar, HWND_TOPMOST, o->ox + x, o->oy + y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// рисуем штрихи кисти: dx/dy — смещение (0 для оверлея, -sel для кроя), width в физ. px
void draw_strokes(HDC dc, const std::vector<Overlay::Stroke>& strokes, int dx, int dy, int width) {
    if (strokes.empty()) return;
    LOGBRUSH lb = {};
    lb.lbStyle = BS_SOLID;
    lb.lbColor = RGB(230, 40, 40);
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, nullptr);
    if (!pen) return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (auto& st : strokes) {
        if (st.pts.empty()) continue;
        if (st.pts.size() == 1) {
            POINT c = st.pts[0];
            int r = width / 2 + 1;
            Ellipse(dc, c.x + dx - r, c.y + dy - r, c.x + dx + r, c.y + dy + r);
        } else {
            std::vector<POINT> pts = st.pts;
            for (auto& p : pts) { p.x += dx; p.y += dy; }
            Polyline(dc, pts.data(), (int)pts.size());
        }
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);
    DeleteObject(pen);
}

void paint_overlay(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dst = BeginPaint(hwnd, &ps);
    Overlay* o = ov;
    if (!o || !o->memDC) { EndPaint(hwnd, &ps); return; }

    BitBlt(o->memDC, 0, 0, o->sw, o->sh, o->dimDC, 0, 0, SRCCOPY);
    if (o->hasSel) {
        RECT s = o->sel;
        BitBlt(o->memDC, s.left, s.top, s.right - s.left, s.bottom - s.top, o->shotDC, s.left, s.top, SRCCOPY);
        draw_strokes(o->memDC, o->strokes, 0, 0, std::max(2, sc(o, 4)));

        HGDIOBJ oldFont = SelectObject(o->memDC, o->font);
        SetBkMode(o->memDC, TRANSPARENT);

        // рамка
        HPEN pen = CreatePen(PS_SOLID, std::max(1, sc(o, 2)), RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(o->memDC, pen);
        HGDIOBJ oldBrm = SelectObject(o->memDC, GetStockObject(NULL_BRUSH));
        Rectangle(o->memDC, s.left, s.top, s.right, s.bottom);
        SelectObject(o->memDC, oldPen);
        DeleteObject(pen);

        // маркеры
        HPEN hpen = CreatePen(PS_SOLID, std::max(1, sc(o, 1)), RGB(255, 255, 255));
        HGDIOBJ pen2 = SelectObject(o->memDC, hpen);
        HBRUSH hbr = CreateSolidBrush(RGB(0, 170, 120));
        HGDIOBJ oldBr2 = SelectObject(o->memDC, hbr);
        int half = std::max(3, sc(o, 5));
        for (int i = 0; i < 8; i++) {
            POINT c = handle_center(s, i);
            Rectangle(o->memDC, c.x - half, c.y - half, c.x + half, c.y + half);
        }
        SelectObject(o->memDC, oldBr2);
        SelectObject(o->memDC, pen2);
        DeleteObject(hbr);
        DeleteObject(hpen);
        SelectObject(o->memDC, oldBrm);

        // плашка «Ш × В»
        wchar_t txt[48];
        swprintf(txt, 48, L"%d \u00D7 %d", s.right - s.left, s.bottom - s.top);
        SIZE ts = {};
        GetTextExtentPoint32W(o->memDC, txt, (int)wcslen(txt), &ts);
        int px = sc(o, 6), py = sc(o, 2);
        int bx = s.left, by = s.top - (ts.cy + py * 2) - sc(o, 4);
        if (by < 0) by = s.top + sc(o, 4);
        if (bx + ts.cx + px * 2 > o->sw) bx = o->sw - (ts.cx + px * 2) - sc(o, 2);
        HBRUSH bg = CreateSolidBrush(RGB(25, 25, 25));
        HGDIOBJ oldB = SelectObject(o->memDC, bg);
        SelectObject(o->memDC, GetStockObject(NULL_PEN));
        Rectangle(o->memDC, bx, by, bx + ts.cx + px * 2, by + ts.cy + py * 2);
        SetTextColor(o->memDC, RGB(255, 255, 255));
        TextOutW(o->memDC, bx + px, by + py, txt, (int)wcslen(txt));
        SelectObject(o->memDC, oldB);
        DeleteObject(bg);
        SelectObject(o->memDC, oldFont);
    }
    BitBlt(dst, 0, 0, o->sw, o->sh, o->memDC, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

void finish_action(int mode) {
    Overlay* o = ov;
    if (!o || !o->hasSel) return;
    RECT s = norm_rect(o->sel);
    int w = s.right - s.left, h = s.bottom - s.top;
    if (w >= 2 && h >= 2 && o->shotBits) {
        RGBQUAD* crop = new (std::nothrow) RGBQUAD[(size_t)w * h];
        if (crop) {
            for (int y = 0; y < h; y++)
                memcpy(crop + (size_t)y * w,
                       o->shotBits + ((size_t)(s.top + y) * o->sw + s.left),
                       (size_t)w * sizeof(RGBQUAD));
            if (!o->strokes.empty()) {
                // впекаем штрихи кисти в крой через временный DIB
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* cbits = nullptr;
                HBITMAP cdib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &cbits, nullptr, 0);
                if (cdib && cbits) {
                    memcpy(cbits, crop, (size_t)w * h * 4);
                    HDC cdc = CreateCompatibleDC(nullptr);
                    HGDIOBJ co = SelectObject(cdc, cdib);
                    draw_strokes(cdc, o->strokes, -s.left, -s.top, std::max(2, sc(o, 4)));
                    memcpy(crop, cbits, (size_t)w * h * 4);
                    SelectObject(cdc, co);
                    DeleteDC(cdc);
                    DeleteObject(cdib);
                }
            }
            Shot sub;
            sub.px = crop;
            sub.w = w;
            sub.h = h;
            export_shot(sub, mode);
            sub.free();
        } else {
            tray_notify(LS(S_APP), L"Не хватило памяти на вырезку");
        }
    }
    o->done = true;
}

void close_overlay() {
    if (ov) ov->done = true;
}

void free_surfaces(Overlay* o) {
    if (o->shotBmp) DeleteObject(o->shotBmp);
    if (o->dimBmp) DeleteObject(o->dimBmp);
    if (o->memBmp) DeleteObject(o->memBmp);
    if (o->shotDC) DeleteDC(o->shotDC);
    if (o->dimDC) DeleteDC(o->dimDC);
    if (o->memDC) DeleteDC(o->memDC);
    if (o->font) DeleteObject(o->font);
    if (o->fontHint) DeleteObject(o->fontHint);
    o->shotBmp = o->dimBmp = o->memBmp = nullptr;
    o->shotDC = o->dimDC = o->memDC = nullptr;
    o->font = o->fontHint = nullptr;
    o->shotBits = nullptr;
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = ov;
    switch (msg) {
        case WM_PAINT:
            paint_overlay(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1; // вся отрисовка в WM_PAINT через двойную буферизацию
        case WM_LBUTTONDOWN: {
            if (!o) return 0;
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            o->lastPos = p;
            if (o->penMode) { // режим кисти: рисуем штрих
                Overlay::Stroke st;
                st.pts.push_back(p);
                o->strokes.push_back(std::move(st));
                o->dragMode = 5;
                SetCapture(hwnd);
                o->captured = true;
                return 0;
            }
            int hit = o->hasSel ? hit_test(o->sel, o->hs, p) : -1;
            if (hit == -2) o->dragMode = 2;
            else if (hit >= 0) { o->dragMode = 3 + hit; o->selStart = o->sel; }
            else { o->dragMode = 1; o->anchor = p; o->hasSel = false; hide_toolbar(o); }
            SetCapture(hwnd);
            o->captured = true;
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!o) return 0;
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (o->captured && o->dragMode == 5) {
                auto& pts = o->strokes.back().pts;
                POINT last = pts.back();
                if (abs(p.x - last.x) > 2 || abs(p.y - last.y) > 2) {
                    pts.push_back(p);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (o->captured && o->dragMode) {
                update_sel_from_drag(o, p);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (!o->captured) {
                if (o->penMode) { SetCursor(LoadCursor(nullptr, IDC_CROSS)); o->hoverHandle = -98; return 0; }
                int hit = o->hasSel ? hit_test(o->sel, o->hs, p) : -1;
                if (hit != o->hoverHandle) {
                    o->hoverHandle = hit;
                    SetCursor(cursor_for_hit(hit));
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!o) return 0;
            if (o->captured) { ReleaseCapture(); o->captured = false; }
            if (o->dragMode == 5) { // завершён штрих кисти
                o->dragMode = 0;
                return 0;
            }
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            update_sel_from_drag(o, p);
            o->dragMode = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (o->hasSel) show_toolbar(o);
            else hide_toolbar(o);
            return 0;
        }
        case WM_RBUTTONUP:
            close_overlay();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (o && o->hasSel && !o->penMode) finish_action(EXP_CLIP_FILE);
            return 0;
        case WM_KEYDOWN:
            if (!o) return 0;
            switch (wp) {
                case VK_ESCAPE:
                    close_overlay();
                    return 0;
                case 'P':
                    o->penMode = !o->penMode;
                    if (o->toolbar) InvalidateRect(o->toolbar, nullptr, FALSE);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case VK_DELETE:
                case VK_BACK:
                    if (o->penMode && !o->strokes.empty()) {
                        o->strokes.pop_back();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case '1':
                    if (o->hasSel) finish_action(EXP_CLIP_FILE);
                    return 0;
                case '2':
                    if (o->hasSel) finish_action(EXP_CLIP_ONLY);
                    return 0;
                case '3':
                    if (o->hasSel) finish_action(EXP_FILE_ONLY);
                    return 0;
                case VK_RETURN:
                    if (o->hasSel) finish_action(EXP_CLIP_FILE);
                    return 0;
                case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
                    if (!o->hasSel) return 0;
                    int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                    RECT r = o->sel;
                    if (wp == VK_LEFT) { r.left -= step; r.right -= step; }
                    if (wp == VK_RIGHT) { r.left += step; r.right += step; }
                    if (wp == VK_UP) { r.top -= step; r.bottom -= step; }
                    if (wp == VK_DOWN) { r.top += step; r.bottom += step; }
                    clamp_rect(r, o->sw, o->sh);
                    o->sel = r;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        case WM_CLOSE:
            close_overlay(); // Alt+F4 по оверлею = отмена, окно закроем при разворачивании цикла
            return 0;
        case WM_DISPLAYCHANGE:
            close_overlay(); // сменился режим экрана — оверлей неактуален
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// индекс кнопки панели под точкой (координаты клиента тулбара); 0 — кисть, 1..3 — действия
int toolbar_button_at(const Overlay* o, POINT p) {
    for (int i = 0; i < 4; i++)
        if (p.x >= o->tbBtns[i].left && p.x < o->tbBtns[i].right &&
            p.y >= o->tbBtns[i].top && p.y < o->tbBtns[i].bottom)
            return i;
    return -1;
}

LRESULT CALLBACK ToolbarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = ov;
    if (!o) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);
            HBRUSH bgb = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(dc, &cr, bgb);
            DeleteObject(bgb);

            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(dc, o->font);
            const wchar_t* labels[4] = {L"✏",
                                         L"1 \u00B7 \u0431\u0443\u0444\u0435\u0440+\u043F\u0430\u043F\u043A\u0430",
                                         L"2 \u00B7 \u0431\u0443\u0444\u0435\u0440",
                                         L"3 \u00B7 \u043F\u0430\u043F\u043A\u0430"};
            for (int i = 0; i < 4; i++) {
                RECT b = o->tbBtns[i];
                COLORREF col = RGB(48, 48, 48);
                if (i == 0 && o->penMode) col = RGB(0, 140, 90);
                else if (i == o->hoverBtn) col = RGB(70, 70, 70);
                HBRUSH bb = CreateSolidBrush(col);
                FillRect(dc, &b, bb);
                DeleteObject(bb);
                SetTextColor(dc, RGB(240, 240, 240));
                DrawTextW(dc, labels[i], -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(dc, oldFont);

            // строка-подсказка
            oldFont = SelectObject(dc, o->fontHint);
            RECT hint = {sc(o, 8), o->tbBtns[0].bottom + sc(o, 4), cr.right - sc(o, 8), o->tbH};
            SetTextColor(dc, RGB(160, 160, 160));
            DrawTextW(dc, LS(S_OV_ESC), -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int i = toolbar_button_at(o, p);
            if (i != o->hoverBtn) {
                o->hoverBtn = i;
                RECT cr;
                GetClientRect(hwnd, &cr);
                InvalidateRect(hwnd, &cr, FALSE);
            }
            if (!o->tracking) {
                o->tracking = true;
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            o->tracking = false;
            if (o->hoverBtn != -1) {
                o->hoverBtn = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int i = toolbar_button_at(o, p);
            if (i == 0) { // переключить режим кисти
                o->penMode = !o->penMode;
                if (o->hwnd) {
                    InvalidateRect(o->hwnd, nullptr, FALSE);
                    SetCursor(LoadCursor(nullptr, o->penMode ? IDC_CROSS : IDC_ARROW));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (i >= 1) {
                finish_action(i);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool make_surfaces(Overlay* o, const Shot& src) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = o->sw;
    bi.bmiHeader.biHeight = -o->sh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bitsBright = nullptr;
    o->shotBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bitsBright, nullptr, 0);
    void* bitsDim = nullptr;
    o->dimBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bitsDim, nullptr, 0);
    if (!o->shotBmp || !o->dimBmp) return false;

    memcpy(bitsBright, src.px, (size_t)o->sw * o->sh * 4);
    // затемнение 60% (коэффициент 40%) — арифметикой без MMX
    RGBQUAD* d = (RGBQUAD*)bitsDim;
    for (size_t i = 0, n = (size_t)o->sw * o->sh; i < n; i++) {
        d[i].rgbBlue = (unsigned char)(src.px[i].rgbBlue * 41 / 100);
        d[i].rgbGreen = (unsigned char)(src.px[i].rgbGreen * 41 / 100);
        d[i].rgbRed = (unsigned char)(src.px[i].rgbRed * 41 / 100);
    }

    o->shotDC = CreateCompatibleDC(nullptr);
    o->dimDC = CreateCompatibleDC(nullptr);
    o->memDC = CreateCompatibleDC(nullptr);
    if (!o->shotDC || !o->dimDC || !o->memDC) return false;
    SelectObject(o->shotDC, o->shotBmp);
    SelectObject(o->dimDC, o->dimBmp);

    o->memBmp = CreateCompatibleBitmap(GetDC(nullptr), o->sw, o->sh);
    if (!o->memBmp) return false;
    SelectObject(o->memDC, o->memBmp);
    o->shotBits = (RGBQUAD*)bitsBright;
    return true;
}

} // namespace

void overlay_register_classes(HINSTANCE inst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = CLS_OVERLAY;
    RegisterClassExW(&wc);

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ToolbarProc;
    wc.lpszClassName = CLS_TOOLBAR;
    RegisterClassExW(&wc);
}

bool overlay_is_open() { return g_overlayOpen; }

void overlay_run(Shot&& shot, const RECT& monRc, UINT dpi) {
    if (g_overlayOpen) {
        shot.free();
        return;
    }
    Overlay tmp;
    ov = &tmp;
    ov->dpi = dpi;
    ov->ox = monRc.left;
    ov->oy = monRc.top;
    ov->sw = monRc.right - monRc.left;
    ov->sh = monRc.bottom - monRc.top;
    ov->hs = sc(ov, 10);
    g_overlayOpen = true;

    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_hwndMain, GWLP_HINSTANCE);
    do {
        if (!make_surfaces(ov, shot)) {
            tray_notify(LS(S_APP), (L"Не удалось подготовить оверлей: " + winerr_text(GetLastError())).c_str());
            break;
        }
        shot.free(); // пиксели уже в shotBmp/dimBmp

        ov->font = CreateFontW(-sc(ov, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ov->fontHint = CreateFontW(-sc(ov, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        // размеры панели по тексту кнопок
        {
            HDC dc = GetDC(nullptr);
            HGDIOBJ oldf = SelectObject(dc, ov->font);
            const wchar_t* labels[3] = {L"1 \u00B7 \u0431\u0443\u0444\u0435\u0440+\u043F\u0430\u043F\u043A\u0430",
                                         L"2 \u00B7 \u0431\u0443\u0444\u0435\u0440",
                                         L"3 \u00B7 \u043F\u0430\u043F\u043A\u0430"};
            int bw = 0;
            for (int i = 0; i < 3; i++) {
                SIZE ts = {};
                GetTextExtentPoint32W(dc, labels[i], (int)wcslen(labels[i]), &ts);
                if (ts.cx > bw) bw = ts.cx;
            }
            SelectObject(dc, oldf);
            ReleaseDC(nullptr, dc);
            int padX = sc(ov, 14), padY = sc(ov, 10), sep = sc(ov, 2);
            bw += padX * 2;
            int bh = sc(ov, 40);
            int penW = sc(ov, 48); // кнопка кисти
            ov->tbBtns[0] = {padX, padY, padX + penW, padY + bh};
            for (int i = 0; i < 3; i++) {
                int x = padX + penW + sep + i * (bw + sep);
                ov->tbBtns[i + 1] = {x, padY, x + bw, padY + bh};
            }
            ov->tbW = padX * 2 + penW + bw * 3 + sep * 3;
            ov->tbH = padY * 2 + bh + sc(ov, 26);
        }

        ov->hwnd = CreateWindowExW(WS_EX_TOPMOST, CLS_OVERLAY, L"", WS_POPUP,
            monRc.left, monRc.top, ov->sw, ov->sh, g_hwndMain, nullptr, inst, nullptr);
        if (!ov->hwnd) {
            tray_notify(LS(S_APP), (L"Не удалось создать оверлей: " + winerr_text(GetLastError())).c_str());
            break;
        }
        ov->toolbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, CLS_TOOLBAR, L"",
            WS_POPUP, 0, 0, ov->tbW, ov->tbH, ov->hwnd, nullptr, inst, nullptr);

        // трюк с Alt снимает foreground lock — иначе SetForegroundWindow может не пустить фокус
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        ShowWindow(ov->hwnd, SW_SHOW);
        SetForegroundWindow(ov->hwnd);
        SetFocus(ov->hwnd);

        MSG m;
        for (;;) {
            BOOL g = GetMessageW(&m, nullptr, 0, 0);
            if (g == 0) { g_quitRequested = true; break; } // WM_QUIT (выход из трея)
            if (g == -1) break;
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (ov->done) break;
        }
    } while (0);

    if (ov->toolbar) DestroyWindow(ov->toolbar);
    if (ov->hwnd) DestroyWindow(ov->hwnd);
    free_surfaces(ov);
    ov = nullptr;
    g_overlayOpen = false;
}
