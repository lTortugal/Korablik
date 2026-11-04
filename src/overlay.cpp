// Оверлей выделения области: «замороженный» затемнённый снимок, мышь, маркеры,
// компактная панель из трёх действий, горячие клавиши 1/2/3/Enter/Esc
#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

static const wchar_t* CLS_OVERLAY = L"WinShotOverlayCls";
static const wchar_t* CLS_TOOLBAR = L"WinShotToolbarCls";

// внешний из main.cpp — скрытое окно приложения (владелец оверлея)
extern HWND g_hwndMain;
extern bool g_quitRequested;

namespace {

// образцы готовых цветов в колонку справа от палитры
const COLORREF kPresets[6] = {RGB(220, 40, 40), RGB(240, 170, 0), RGB(40, 170, 60),
                              RGB(0, 160, 240), RGB(0, 0, 0), RGB(255, 255, 255)};

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
    COLORREF penColor = RGB(0, 0, 140);
    struct Stroke { std::vector<POINT> pts; COLORREF color = RGB(0, 0, 140); };
    std::vector<Stroke> strokes;
    // круговая палитра: колесо (угол=тон, радиус=насыщенность) + полоса яркости + превью
    double hue = 240, sat = 1.0, val = 0.55;
    HDC wheelDC = nullptr, barDC = nullptr;
    HBITMAP wheelBmp = nullptr, barBmp = nullptr;
    RECT wheelRect = {}, barRect = {}, previewRect = {};
    int pick = 0; // 0 нет, 1 колесо, 2 полоса
    // готовые цвета под превью: клик ставит цвет и переставляет колесо/полосу
    RECT presetRects[6] = {};
    int presetCount = 0;

    // панель
    RECT tbBtns[5] = {}; // 0 — кисть, 1 — отмена штриха, 2..4 — действия 1/2/3
    int tbW = 0, tbH = 0;
    int hoverBtn = -1;
    bool tracking = false;

    int hs = 14; // допуск попадания в маркер, физ. px
};

Overlay* ov = nullptr;
bool g_overlayOpen = false;

int sc(Overlay* o, int logical) { return MulDiv(logical, (int)o->dpi, 96); }

// ---------- круговая палитра ----------
static const int PAL_M = 256; // размер мастера колеса/полосы в пикселях

static COLORREF hsv_rgb(double h, double s, double v) {
    if (h < 0) h += 360.0;
    double c = v * s;
    double hh = h / 60.0;
    double x = c * (1.0 - fabs(fmod(hh, 2.0) - 1.0));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    int seg = (int)floor(hh) % 6;
    switch (seg) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    return RGB((int)((r + m) * 255 + 0.5), (int)((g + m) * 255 + 0.5), (int)((b + m) * 255 + 0.5));
}

// обратно из цвета в HSV — чтобы готовый образец двигал маркеры колеса и полосы
static void rgb_hsv(COLORREF c, double& h, double& s, double& v) {
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx;
    s = mx == 0 ? 0 : d / mx;
    if (d == 0) h = 0;
    else if (mx == r) h = 60.0 * fmod((g - b) / d + 6.0, 6.0);
    else if (mx == g) h = 60.0 * ((b - r) / d + 2.0);
    else h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0) h += 360.0;
}

// колесо зависит только от яркости; полоса — от тона/насыщенности
static void palette_build_wheel(Overlay* o) {
    if (o->wheelBmp) {
        if (o->wheelDC) SelectObject(o->wheelDC, nullptr);
        DeleteObject(o->wheelBmp);
        o->wheelBmp = nullptr;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = PAL_M;
    bi.bmiHeader.biHeight = -PAL_M;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    o->wheelBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!o->wheelBmp || !bits) return;
    if (!o->wheelDC) o->wheelDC = CreateCompatibleDC(nullptr);
    SelectObject(o->wheelDC, o->wheelBmp);
    RGBQUAD* px = (RGBQUAD*)bits;
    double c = (PAL_M - 1) / 2.0;
    for (int y = 0; y < PAL_M; y++) {
        for (int i = 0; i < PAL_M; i++) {
            double dx = (i - c) / c, dy = (y - c) / c;
            double r2 = dx * dx + dy * dy;
            RGBQUAD& q = px[(size_t)y * PAL_M + i];
            if (r2 <= 1.0) {
                double ang = atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
                COLORREF col = hsv_rgb(ang, sqrt(r2), o->val);
                q.rgbRed = GetRValue(col);
                q.rgbGreen = GetGValue(col);
                q.rgbBlue = GetBValue(col);
                q.rgbReserved = 255;
            } else {
                q.rgbRed = q.rgbGreen = q.rgbBlue = q.rgbReserved = 0;
            }
        }
    }
}

static void palette_build_bar(Overlay* o) {
    if (o->barBmp) {
        if (o->barDC) SelectObject(o->barDC, nullptr);
        DeleteObject(o->barBmp);
        o->barBmp = nullptr;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = -PAL_M;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    o->barBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!o->barBmp || !bits) return;
    if (!o->barDC) o->barDC = CreateCompatibleDC(nullptr);
    SelectObject(o->barDC, o->barBmp);
    RGBQUAD* px = (RGBQUAD*)bits;
    for (int y = 0; y < PAL_M; y++) {
        COLORREF col = hsv_rgb(o->hue, o->sat, 1.0 - y / (double)(PAL_M - 1));
        for (int x = 0; x < 16; x++) {
            RGBQUAD& q = px[(size_t)y * 16 + x];
            q.rgbRed = GetRValue(col);
            q.rgbGreen = GetGValue(col);
            q.rgbBlue = GetBValue(col);
            q.rgbReserved = 255;
        }
    }
}

static void palette_pick(Overlay* o, POINT p) {
    if (o->pick == 1) {
        double c = (o->wheelRect.right - o->wheelRect.left) / 2.0;
        double dx = (p.x - (o->wheelRect.left + c)) / c;
        double dy = (p.y - (o->wheelRect.top + c)) / c;
        double r2 = dx * dx + dy * dy;
        double ns = r2 > 1.0 ? 1.0 : sqrt(r2);
        double nh = atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
        if (nh != o->hue || ns != o->sat) {
            o->hue = nh < 0 ? nh + 360 : nh;
            o->sat = ns;
            palette_build_bar(o);
            o->penColor = hsv_rgb(o->hue, o->sat, o->val);
            if (o->toolbar) InvalidateRect(o->toolbar, nullptr, FALSE);
        }
    } else if (o->pick == 2) {
        int h = o->barRect.bottom - o->barRect.top;
        double nv = 1.0 - (p.y - o->barRect.top) / (double)h;
        if (nv < 0) nv = 0;
        if (nv > 1) nv = 1;
        if (nv != o->val) {
            o->val = nv;
            palette_build_wheel(o);
            o->penColor = hsv_rgb(o->hue, o->sat, o->val);
            if (o->toolbar) InvalidateRect(o->toolbar, nullptr, FALSE);
        }
    }
}

static void palette_free(Overlay* o) {
    if (o->wheelDC) SelectObject(o->wheelDC, nullptr);
    if (o->barDC) SelectObject(o->barDC, nullptr);
    if (o->wheelBmp) DeleteObject(o->wheelBmp);
    if (o->barBmp) DeleteObject(o->barBmp);
    if (o->wheelDC) DeleteDC(o->wheelDC);
    if (o->barDC) DeleteDC(o->barDC);
    o->wheelBmp = o->barBmp = nullptr;
    o->wheelDC = o->barDC = nullptr;
}

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
    if (o->hwnd) InvalidateRect(o->hwnd, nullptr, FALSE); // под панелью остаётся оверлей
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
    for (auto& st : strokes) {
        if (st.pts.empty()) continue;
        LOGBRUSH lb = {};
        lb.lbStyle = BS_SOLID;
        lb.lbColor = st.color;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, nullptr);
        if (!pen) continue;
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (st.pts.size() == 1) {
            POINT c = st.pts[0];
            int r = width / 2 + 1;
            Ellipse(dc, c.x + dx - r, c.y + dy - r, c.x + dx + r, c.y + dy + r);
        } else {
            std::vector<POINT> pts = st.pts;
            for (auto& p : pts) { p.x += dx; p.y += dy; }
            Polyline(dc, pts.data(), (int)pts.size());
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);
        DeleteObject(pen);
    }
}

void undo_stroke(Overlay* o) {
    if (o->strokes.empty()) return;
    o->strokes.pop_back();
    if (o->hwnd) InvalidateRect(o->hwnd, nullptr, FALSE);
    if (o->toolbar) InvalidateRect(o->toolbar, nullptr, FALSE); // кнопка гаснет, когда штрихов не осталось
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
        HBRUSH hbr = CreateSolidBrush(RGB(0, 0, 140));
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

        // плашка «Ш × В» (при активном штрихе не мешаем рисовать — прячем)
        if (o->dragMode != 5) {
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
        }
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
            if (o->penMode) { // режим кисти: рисуем штрих текущим цветом
                hide_toolbar(o); // панель перекрывает место под рисунком — прячем на время штриха
                Overlay::Stroke st;
                st.pts.push_back(p);
                st.color = o->penColor;
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
                if (o->penMode) {
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                    o->hoverHandle = -98;
                    // панель прячем на время штриха; если WM_LBUTTONUP её не вернул
                    // (клик по панели не активирует оверлей) — возвращаем при первом движении
                    if (o->toolbar && !IsWindowVisible(o->toolbar)) show_toolbar(o);
                    return 0;
                }
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
            if (o->hasSel && o->dragMode != 5) show_toolbar(o);
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
                    if (o->penMode) undo_stroke(o);
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

// индекс кнопки панели под точкой (координаты клиента тулбара); 0 — кисть, 1 — отмена штриха, 2..4 — действия
int toolbar_button_at(const Overlay* o, POINT p) {
    for (int i = 0; i < 5; i++)
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
            const wchar_t* labels[5] = {L"✏", L"↺",
                                         L"1 \u00B7 \u0431\u0443\u0444\u0435\u0440+\u043F\u0430\u043F\u043A\u0430",
                                         L"2 \u00B7 \u0431\u0443\u0444\u0435\u0440",
                                         L"3 \u00B7 \u043F\u0430\u043F\u043A\u0430"};
            for (int i = 0; i < 5; i++) {
                RECT b = o->tbBtns[i];
                COLORREF col = RGB(48, 48, 48);
                if (i == 0 && o->penMode) col = o->penColor;
                else if (i == o->hoverBtn) col = RGB(70, 70, 70);
                HBRUSH bb = CreateSolidBrush(col);
                FillRect(dc, &b, bb);
                DeleteObject(bb);
                // «↺» без штрихов неактивна
                SetTextColor(dc, (i == 1 && o->strokes.empty()) ? RGB(110, 110, 110) : RGB(240, 240, 240));
                DrawTextW(dc, labels[i], -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(dc, oldFont);

            // круговая палитра: колесо, полоса яркости, превью выбранного цвета
            if (o->wheelDC && o->barDC) {
                int prevMode = SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, o->wheelRect.left, o->wheelRect.top,
                           o->wheelRect.right - o->wheelRect.left, o->wheelRect.bottom - o->wheelRect.top,
                           o->wheelDC, 0, 0, PAL_M, PAL_M, SRCCOPY);
                StretchBlt(dc, o->barRect.left, o->barRect.top,
                           o->barRect.right - o->barRect.left, o->barRect.bottom - o->barRect.top,
                           o->barDC, 0, 0, 16, PAL_M, SRCCOPY);
                SetStretchBltMode(dc, prevMode);
                // маркеры: точка на колесе и полосе
                double wc = (o->wheelRect.right - o->wheelRect.left) / 2.0;
                double ang = o->hue * 3.14159265358979323846 / 180.0;
                int mx = (int)(o->wheelRect.left + wc + cos(ang) * o->sat * wc);
                int my = (int)(o->wheelRect.top + wc - sin(ang) * o->sat * wc);
                HPEN dotPen = CreatePen(PS_SOLID, sc(o, 2), RGB(255, 255, 255));
                HGDIOBJ op = SelectObject(dc, dotPen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Ellipse(dc, mx - sc(o, 4), my - sc(o, 4), mx + sc(o, 4), my + sc(o, 4));
                int by = o->barRect.top + (int)((1.0 - o->val) * (o->barRect.bottom - o->barRect.top));
                Rectangle(dc, o->barRect.left - sc(o, 2), by - sc(o, 3), o->barRect.right + sc(o, 2), by + sc(o, 3));
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(dotPen);
            }
            HBRUSH pb = CreateSolidBrush(o->penColor);
            FillRect(dc, &o->previewRect, pb);
            DeleteObject(pb);
            HPEN pbrd = CreatePen(PS_SOLID, sc(o, 1), RGB(120, 120, 120));
            HGDIOBJ opb = SelectObject(dc, pbrd);
            HGDIOBJ obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, o->previewRect.left, o->previewRect.top, o->previewRect.right, o->previewRect.bottom);
            SelectObject(dc, opb);
            SelectObject(dc, obb);
            DeleteObject(pbrd);

            for (int i = 0; i < o->presetCount; i++) {
                HBRUSH sb = CreateSolidBrush(kPresets[i]);
                FillRect(dc, &o->presetRects[i], sb);
                DeleteObject(sb);
            }

            // строка-подсказка под палитрой
            oldFont = SelectObject(dc, o->fontHint);
            RECT hint = {sc(o, 8), o->wheelRect.bottom + sc(o, 3), cr.right - sc(o, 8), o->tbH};
            SetTextColor(dc, RGB(160, 160, 160));
            DrawTextW(dc, LS(S_OV_ESC), -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!o) return 0;
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int i = 0; i < o->presetCount; i++) { // готовые цвета: ставят цвет и включают кисть
                const RECT& pr = o->presetRects[i];
                if (p.x >= pr.left && p.x < pr.right && p.y >= pr.top && p.y < pr.bottom) {
                    o->penColor = kPresets[i];
                    rgb_hsv(kPresets[i], o->hue, o->sat, o->val);
                    palette_build_wheel(o);
                    palette_build_bar(o);
                    if (!o->penMode) { o->penMode = true; if (o->hwnd) InvalidateRect(o->hwnd, nullptr, FALSE); }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            // колесо: круглая зона; полоса: прямоугольник
            double wc = (o->wheelRect.right - o->wheelRect.left) / 2.0;
            double dx = p.x - (o->wheelRect.left + wc), dy = p.y - (o->wheelRect.top + wc);
            if (wc > 0 && dx * dx + dy * dy <= wc * wc) {
                o->pick = 1;
                if (!o->penMode) { o->penMode = true; if (o->hwnd) InvalidateRect(o->hwnd, nullptr, FALSE); }
                SetCapture(hwnd);
                palette_pick(o, p);
                return 0;
            }
            if (p.x >= o->barRect.left - sc(o, 3) && p.x <= o->barRect.right + sc(o, 3) &&
                p.y >= o->barRect.top && p.y <= o->barRect.bottom) {
                o->pick = 2;
                if (!o->penMode) { o->penMode = true; if (o->hwnd) InvalidateRect(o->hwnd, nullptr, FALSE); }
                SetCapture(hwnd);
                palette_pick(o, p);
                return 0;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (o && o->pick) {
                palette_pick(o, p);
                return 0;
            }
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
            if (o->pick) { // завершение выбора цвета
                palette_pick(o, p);
                o->pick = 0;
                ReleaseCapture();
                return 0;
            }
            int i = toolbar_button_at(o, p);
            if (i == 0) { // переключить режим кисти
                o->penMode = !o->penMode;
                if (o->hwnd) {
                    InvalidateRect(o->hwnd, nullptr, FALSE);
                    SetCursor(LoadCursor(nullptr, o->penMode ? IDC_CROSS : IDC_ARROW));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (i == 1) { undo_stroke(o); return 0; }
            if (i >= 2) { finish_action(i - 1); return 0; }
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

        // размеры панели: три ряда одинаковой ширины — ✏ ↺ действие / действие действие / палитра
        {
            HDC dc = GetDC(nullptr);
            HGDIOBJ oldf = SelectObject(dc, ov->font);
            SIZE ts = {};
            const wchar_t* longest = L"1 \u00B7 \u0431\u0443\u0444\u0435\u0440+\u043F\u0430\u043F\u043A\u0430";
            GetTextExtentPoint32W(dc, longest, (int)wcslen(longest), &ts);
            SelectObject(dc, oldf);
            ReleaseDC(nullptr, dc);

            int padX = sc(ov, 14), padY = sc(ov, 10), sep = sc(ov, 2);
            int bh = sc(ov, 40);
            int smallW = sc(ov, 48); // узкие кнопки: кисть и отмена штриха
            int barW = sc(ov, 22);
            int gap = sc(ov, 12);

            int actMin = std::max(sc(ov, 64), (int)ts.cx + padX * 2);
            int swSz = sc(ov, 20); // превью и готовые цвета — квадраты одного размера
            int palMin = sc(ov, 150) + gap * 2 + barW + swSz; // колесо в минимуме + полоса + квадрат
            int innerW = std::max(smallW * 2 + sep * 2 + actMin, palMin);
            if ((innerW - sep) % 2) innerW++; // чтобы два нижних действия были ровно равными
            ov->tbW = innerW + padX * 2;

            int row2Y = padY + bh + sep;
            ov->tbBtns[0] = {padX, padY, padX + smallW, padY + bh};
            ov->tbBtns[1] = {padX + smallW + sep, padY, padX + smallW * 2 + sep, padY + bh};
            ov->tbBtns[2] = {padX + smallW * 2 + sep * 2, padY, padX + innerW, padY + bh};
            int half = (innerW - sep) / 2;
            ov->tbBtns[3] = {padX, row2Y, padX + half, row2Y + bh};
            ov->tbBtns[4] = {padX + half + sep, row2Y, padX + half * 2 + sep, row2Y + bh};

            // круговая палитра: колесо дотянуто до правого края, рядом полоса яркости и превью
            int palTop = row2Y + bh + sc(ov, 8);
            int wheelSz = std::min(sc(ov, 200), innerW - (gap * 2 + barW * 2));
            if (wheelSz < sc(ov, 120)) wheelSz = sc(ov, 120);
            ov->wheelRect = {padX, palTop, padX + wheelSz, palTop + wheelSz};
            ov->barRect = {ov->wheelRect.right + gap, palTop, ov->wheelRect.right + gap + barW, palTop + wheelSz};
            ov->previewRect = {ov->barRect.right + gap, palTop, ov->barRect.right + gap + swSz, palTop + swSz};
            // готовые цвета — в ту же колонку под превью, сколько влезает по высоте колеса
            int colTop = ov->previewRect.bottom + sc(ov, 5);
            int colBottom = palTop + wheelSz;
            ov->presetCount = 0;
            for (int y = colTop; y + swSz <= colBottom && ov->presetCount < 6; y += swSz + sc(ov, 5)) {
                ov->presetRects[ov->presetCount] = {ov->previewRect.left, y, ov->previewRect.left + swSz, y + swSz};
                ov->presetCount++;
            }
            ov->tbH = palTop + wheelSz + sc(ov, 24) + padY;
            palette_build_wheel(ov);
            palette_build_bar(ov);
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
    palette_free(ov);
    free_surfaces(ov);
    ov = nullptr;
    g_overlayOpen = false;
}
