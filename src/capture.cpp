// Захват монитора под курсором через BitBlt/GDI (быстро и без лишних DLL)
#include "app.h"

// GetDpiForMonitor — из shcore.dll (есть в Windows 8.1+), грузим динамически,
// чтобы не тянуть лишние импорта и не упасть на редких конфигурациях
typedef HRESULT(WINAPI* GetDpiForMonitor_fn)(HMONITOR, int /*MDT_EFFECTIVE_DPI*/, UINT*, UINT*);

static UINT monitor_dpi(HMONITOR hm) {
    UINT dpiX = 96, dpiY = 96;
    static HMODULE shcore = nullptr;
    static GetDpiForMonitor_fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shcore) fn = (GetDpiForMonitor_fn)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    if (fn) fn(hm, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
    if (dpiX < 96) dpiX = 96;
    return dpiX;
}

UINT monitor_dpi_at_cursor() {
    POINT p;
    if (!GetCursorPos(&p)) return 96;
    return monitor_dpi(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST));
}

bool capture_cursor_monitor(Shot& out, RECT& rcMon, UINT& outDpi, std::wstring& err) {
    out.px = nullptr; out.w = out.h = 0;

    POINT cur;
    if (!GetCursorPos(&cur)) {
        err = L"Не удалось получить позицию курсора";
        return false;
    }
    HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO*)&mi)) {
        err = L"Монитор под курсором не найден";
        return false;
    }
    rcMon = mi.rcMonitor;
    outDpi = monitor_dpi(hm);

    const int w = rcMon.right - rcMon.left;
    const int h = rcMon.bottom - rcMon.top;
    if (w <= 0 || h <= 0) {
        err = L"Некорректные размеры монитора";
        return false;
    }

    // DC конкретного монитора: координаты начинаются с 0,0 и это физические пиксели.
    // Если создать не удалось — берём DC всего экрана со смещением монитора.
    HDC src = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
    bool fromScreen = false;
    int srcX = 0, srcY = 0;
    if (!src) {
        src = GetDC(nullptr);
        srcX = rcMon.left;
        srcY = rcMon.top;
        fromScreen = true;
    }

    HDC mem = src ? CreateCompatibleDC(src) : nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = (mem && src) ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        // CAPTUREBLT — чтобы снимать DXG-слои (водяные знаки яркости и т.п.)
        ok = BitBlt(mem, 0, 0, w, h, src, srcX, srcY, SRCCOPY | CAPTUREBLT) != FALSE;
        if (ok) {
            // копируем пиксели из DIB во владение Shot (сам DIB будет удалён)
            RGBQUAD* buf = new (std::nothrow) RGBQUAD[(size_t)w * h];
            if (buf) {
                memcpy(buf, bits, (size_t)w * h * sizeof(RGBQUAD));
                out.px = buf;
                out.w = w;
                out.h = h;
            } else {
                ok = false;
                err = L"Не хватило памяти на снимок";
            }
        }
        SelectObject(mem, old);
    }
    if (!ok && err.empty())
        err = L"BitBlt не удалось: " + winerr_text(GetLastError());

    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (src) {
        if (fromScreen) ReleaseDC(nullptr, src);
        else DeleteDC(src);
    }
    if (!out.px) out.free();
    return ok && out.px != nullptr;
}
