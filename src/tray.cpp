// Иконка в трее, контекстное меню, уведомления, диалог смены хоткеев, окно настроек
#include "app.h"
#include <ole2.h> // CoTaskMemFree для SHBrowseForFolder

extern HWND g_hwndMain;

static NOTIFYICONDATAW g_nid = {};
static HICON g_icon = nullptr;
static bool g_trayAdded = false;

#define IDI_APP_ICON 101 // ресурс из app.rc (ship.ico)

static void tray_refresh_tip() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_TIP;
    lstrcpynW(n.szTip, LS(S_APP), 128);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void tray_add(HWND owner) {
    int cx = GetSystemMetrics(SM_CXSMICON);
    if (cx < 16) cx = 16;
    g_icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
                               IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
    if (!g_icon)
        g_icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_icon) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, LS(S_APP), 128);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    NOTIFYICONDATAW v = g_nid;
    v.uFlags = NIF_SHOWTIP;
    v.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &v);
}

void tray_remove() {
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}

void tray_notify(const wchar_t* title, const wchar_t* text) {
    if (!g_trayAdded || !g_nid.hWnd) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    lstrcpynW(n.szInfoTitle, title, (int)(sizeof(n.szInfoTitle) / sizeof(wchar_t)));
    lstrcpynW(n.szInfo, text, (int)(sizeof(n.szInfo) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------- диалог перехвата клавиши ----------------

static const wchar_t* CLS_HK = L"KorablHkDlgCls";

struct HkDlg {
    HWND hwnd = nullptr;
    bool done = false;
    bool ok = false;
    int which = 1;
    UINT vk = 0;
    UINT mods = 0;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    UINT dpi = 96;
};
static HkDlg* hkd = nullptr;

bool tray_hk_dialog_open() { return hkd != nullptr; }

static int hsc(HkDlg* d, int logical) { return MulDiv(logical, (int)d->dpi, 96); }

// vk+mods -> строка для ini; false, если клавиша не поддерживается
static bool vk_to_text(UINT vk, UINT mods, std::wstring& out) {
    out.clear();
    if (mods & MOD_CONTROL) out += L"CTRL+";
    if (mods & MOD_ALT) out += L"ALT+";
    if (mods & MOD_SHIFT) out += L"SHIFT+";
    if (mods & MOD_WIN) out += L"WIN+";
    wchar_t buf[24];
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf(buf, 24, L"F%d", vk - VK_F1 + 1);
        out += buf;
    } else if (vk == VK_SNAPSHOT) out += L"PRINTSCREEN";
    else if (vk == VK_SPACE) out += L"SPACE";
    else if (vk == VK_RETURN) out += L"ENTER";
    else if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9')) out += (wchar_t)vk;
    else return false;
    return true;
}

static std::wstring join_raw(const std::vector<ParsedHotkey>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L", ";
        s += v[i].raw;
    }
    return s;
}

static void hk_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldF = SelectObject(dc, hkd->font);

    RECT r1 = {hsc(hkd, 12), hsc(hkd, 14), cr.right - hsc(hkd, 12), hsc(hkd, 48)};
    SetTextColor(dc, RGB(255, 255, 255));
    std::wstring t1 = hkd->which == 1 ? LS(S_DLG_FULL)
                    : (hkd->which == 2 ? LS(S_DLG_REGION) : LS(S_DLG_REGION2));
    DrawTextW(dc, t1.c_str(), -1, &r1, DT_CENTER | DT_SINGLELINE);

    std::wstring cur = std::wstring(LS(S_DLG_NOW)) + join_raw(hkd->which == 1 ? g_cfg.hkFull : g_cfg.hkRegion);
    if (cur.empty()) cur = std::wstring(LS(S_DLG_NOW)) + L"-";
    RECT r2 = {r1.left, hsc(hkd, 52), r1.right, hsc(hkd, 76)};
    SetTextColor(dc, RGB(150, 150, 150));
    HGDIOBJ oldS = SelectObject(dc, hkd->fontSmall);
    DrawTextW(dc, cur.c_str(), -1, &r2, DT_CENTER | DT_SINGLELINE);

    RECT r3 = {r1.left, hsc(hkd, 84), r1.right, hsc(hkd, 112)};
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, hkd->font);
    DrawTextW(dc, LS(S_DLG_PRESS), -1, &r3, DT_CENTER | DT_SINGLELINE);

    RECT r4 = {r1.left, hsc(hkd, 118), r1.right, hsc(hkd, 140)};
    SetTextColor(dc, RGB(150, 150, 150));
    SelectObject(dc, hkd->fontSmall);
    DrawTextW(dc, LS(S_DLG_ESC), -1, &r4, DT_CENTER | DT_SINGLELINE);

    SelectObject(dc, oldF);
    SelectObject(dc, oldS);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK HkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HkDlg* d = hkd;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT:
            hk_paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE &&
                !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
                !(GetKeyState(VK_SHIFT) & 0x8000)) {
                d->done = true;
                return 0;
            }
            // голые модификаторы — ждём настоящую клавишу
            if (wp == VK_CONTROL || wp == VK_SHIFT || wp == VK_MENU ||
                wp == VK_LWIN || wp == VK_RWIN || wp == VK_CAPITAL || wp == VK_NUMLOCK ||
                wp == VK_SCROLL)
                return 0;
            {
                UINT mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
                d->vk = (UINT)wp;
                d->mods = mods;
                d->ok = true;
                d->done = true;
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_RBUTTONUP:
        case WM_CLOSE:
            d->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void hotkey_edit_dialog(int which) {
    if (overlay_is_open() || hkd) return;

    static bool cls_registered = false;
    if (!cls_registered) {
        cls_registered = true;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = HkProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = CLS_HK;
        RegisterClassExW(&wc);
    }

    HkDlg tmp;
    hkd = &tmp;
    tmp.which = which;
    tmp.dpi = monitor_dpi_at_cursor();
    hotkeys_pause(); // занятые клавиши (например F18) должны доходить до диалога

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    int w = hsc(&tmp, 420), h = hsc(&tmp, 152);
    int x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - w) / 2;
    int y = mi.rcMonitor.top + ((mi.rcMonitor.bottom - mi.rcMonitor.top) - h) / 2;

    tmp.font = CreateFontW(-hsc(&tmp, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    tmp.fontSmall = CreateFontW(-hsc(&tmp, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    tmp.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, CLS_HK, L"",
        WS_POPUP | WS_BORDER, x, y, w, h, g_hwndMain, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tmp.hwnd) {
        hkd = nullptr;
        return;
    }

    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    ShowWindow(tmp.hwnd, SW_SHOW);
    SetForegroundWindow(tmp.hwnd);
    SetFocus(tmp.hwnd);
    for (int i = 0; i < 6 && GetForegroundWindow() != tmp.hwnd; i++) {
        Sleep(50);
        SetForegroundWindow(tmp.hwnd);
    }

    MSG m;
    while (!tmp.done) {
        BOOL g = GetMessageW(&m, nullptr, 0, 0);
        if (g == 0) { tmp.done = true; break; }
        if (g == -1) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    DestroyWindow(tmp.hwnd);
    DeleteObject(tmp.font);
    DeleteObject(tmp.fontSmall);
    hkd = nullptr;
    hotkeys_resume();

    if (!tmp.ok) return;

    std::wstring s;
    if (!vk_to_text(tmp.vk, tmp.mods, s)) {
        tray_notify(LS(S_APP), LS(S_ERR_BAD_KEY));
        return;
    }
    // быстрая смена через меню трея: перезаписываем первую клавишу списка, остальные сохраняем
    const std::vector<ParsedHotkey>& vec = which == 1 ? g_cfg.hkFull : g_cfg.hkRegion;
    std::wstring newVal = s;
    for (size_t i = 1; i < vec.size(); i++) newVal += L", " + vec[i].raw;
    WritePrivateProfileStringW(L"hotkeys", which == 1 ? L"full_monitor" : L"region",
                               newVal.c_str(), g_cfg.iniPath.c_str());
    WritePrivateProfileStringW(L"hotkeys", L"region_secondary", nullptr, g_cfg.iniPath.c_str()); // легаси больше не нужно
    config_reload_hotkeys();
    hotkeys_apply();
    std::wstring what = which == 1 ? LS(S_HK_SET_FULL) : LS(S_HK_SET_R1);
    tray_notify(LS(S_APP), (what + s).c_str());
}

// ---------------- окно настроек ----------------

static const wchar_t* CLS_SETTINGS = L"KorablSettingsCls";

#define IDC_BROWSE   201
#define IDC_SAVE     202
#define IDC_CANCEL   203
#define IDC_EDIT_DIR 204
#define IDC_LANG_RU  220
#define IDC_LANG_EN  221
#define IDC_KEY0     300 // 300 + col*10 + slot
#define IDC_DEL0     400 // 400 + col*10 + slot
#define IDC_ADD0     500 // 500 + col
#define MAXSLOTS     4

struct Settings {
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    HWND btnKey[2][MAXSLOTS] = {};
    HWND btnDel[2][MAXSLOTS] = {};
    HWND btnAdd[2] = {};
    std::wstring list[2][MAXSLOTS];
    HWND rdoRu = nullptr;
    HWND rdoEn = nullptr;
    int capCol = -1, capSlot = -1;
    bool done = false;
    HFONT font = nullptr;
    UINT dpi = 96;
};
static Settings* st = nullptr;

static int ssc(Settings* s, int logical) { return MulDiv(logical, (int)s->dpi, 96); }

static HWND mk(Settings* s, const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, int id, HINSTANCE inst) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             ssc(s, x), ssc(s, y), ssc(s, w), ssc(s, h),
                             s->hwnd, (HMENU)(INT_PTR)id, inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)s->font, TRUE);
    return c;
}

static void settings_refresh(Settings* s) {
    for (int col = 0; col < 2; col++) {
        for (int i = 0; i < MAXSLOTS; i++) {
            bool empty = s->list[col][i].empty();
            bool capturing = (s->capCol == col && s->capSlot == i);
            ShowWindow(s->btnKey[col][i], capturing || !empty ? SW_SHOW : SW_HIDE);
            ShowWindow(s->btnDel[col][i], empty ? SW_HIDE : SW_SHOW);
            if (capturing) SetWindowTextW(s->btnKey[col][i], LS(S_DLG_PRESSING));
            else if (!empty) SetWindowTextW(s->btnKey[col][i], s->list[col][i].c_str());
        }
    }
}

static void settings_start_capture(Settings* s, int col, int slot) {
    s->capCol = col;
    s->capSlot = slot;
    settings_refresh(s);
    SetFocus(s->hwnd); // клавиши ловит родитель
}

static std::wstring join_list(const std::wstring list[MAXSLOTS]) {
    std::wstring s;
    for (int i = 0; i < MAXSLOTS; i++) {
        if (list[i].empty()) continue;
        if (!s.empty()) s += L", ";
        s += list[i];
    }
    return s;
}

static void settings_save(Settings* s) {
    int len = GetWindowTextLengthW(s->edit);
    std::wstring folder(len + 1, L'\0');
    len = GetWindowTextW(s->edit, &folder[0], (int)folder.size());
    folder.resize(len);
    if (!folder.empty() && folder != g_cfg.shotsFolder) {
        WritePrivateProfileStringW(L"general", L"screens_folder", folder.c_str(), g_cfg.iniPath.c_str());
        g_cfg.shotsFolder = folder;
    }
    std::wstring full = join_list(s->list[0]);
    std::wstring reg = join_list(s->list[1]);
    if (full != join_raw(g_cfg.hkFull) || reg != join_raw(g_cfg.hkRegion)) {
        WritePrivateProfileStringW(L"hotkeys", L"full_monitor", full.c_str(), g_cfg.iniPath.c_str());
        WritePrivateProfileStringW(L"hotkeys", L"region", reg.c_str(), g_cfg.iniPath.c_str());
        WritePrivateProfileStringW(L"hotkeys", L"region_secondary", nullptr, g_cfg.iniPath.c_str());
        config_reload_hotkeys();
        hotkeys_apply();
    }
    bool toEn = SendMessageW(s->rdoEn, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (toEn != (g_lang == LANG_EN)) {
        lang_set(toEn ? L"en" : L"ru");
        WritePrivateProfileStringW(L"general", L"language", toEn ? L"en" : L"ru", g_cfg.iniPath.c_str());
        tray_refresh_tip();
    }
    tray_notify(LS(S_APP), LS(S_SET_SAVED));
    s->done = true;
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Settings* s = st;
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE:
            s->done = true;
            return 0;
        case WM_KEYDOWN:
            if (s->capCol >= 0) {
                if (wp == VK_DELETE || wp == VK_BACK) { // очистить слот
                    s->list[s->capCol][s->capSlot].clear();
                    s->capCol = s->capSlot = -1;
                    settings_refresh(s);
                    return 0;
                }
                if (wp == VK_CONTROL || wp == VK_SHIFT || wp == VK_MENU ||
                    wp == VK_LWIN || wp == VK_RWIN)
                    return 0;
                UINT mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
                std::wstring txt;
                if (vk_to_text((UINT)wp, mods, txt)) {
                    s->list[s->capCol][s->capSlot] = txt;
                    s->capCol = s->capSlot = -1;
                    settings_refresh(s);
                }
            }
            return 0;
        case WM_COMMAND: {
            WORD id = LOWORD(wp);
            if (id == IDCANCEL) {
                if (s->capCol >= 0) { s->capCol = s->capSlot = -1; settings_refresh(s); }
                else s->done = true;
                return 0;
            }
            if (id == IDC_BROWSE) {
                BROWSEINFOW bi = {};
                bi.hwndOwner = hwnd;
                bi.lpszTitle = LS(S_BROWSE_TITLE);
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t path[MAX_PATH * 2] = {};
                    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(s->edit, path);
                    CoTaskMemFree(pidl);
                }
                return 0;
            }
            if (id == IDC_SAVE) { settings_save(s); return 0; }
            if (id == IDC_CANCEL) { s->done = true; return 0; }
            if (id >= IDC_KEY0 && id < IDC_KEY0 + 40) {
                int col = (id - IDC_KEY0) / 10, slot = (id - IDC_KEY0) % 10;
                if (col < 2 && slot < MAXSLOTS) settings_start_capture(s, col, slot);
                return 0;
            }
            if (id >= IDC_DEL0 && id < IDC_DEL0 + 40) {
                int col = (id - IDC_DEL0) / 10, slot = (id - IDC_DEL0) % 10;
                if (col < 2 && slot < MAXSLOTS) {
                    for (int i = slot; i < MAXSLOTS - 1; i++) s->list[col][i] = s->list[col][i + 1];
                    s->list[col][MAXSLOTS - 1].clear();
                    if (s->capCol == col && s->capSlot == slot) s->capCol = s->capSlot = -1;
                    settings_refresh(s);
                }
                return 0;
            }
            if (id >= IDC_ADD0 && id < IDC_ADD0 + 2) {
                int col = id - IDC_ADD0;
                for (int i = 0; i < MAXSLOTS; i++) {
                    if (s->list[col][i].empty()) { settings_start_capture(s, col, i); return 0; }
                }
                return 0;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void settings_open() {
    if (overlay_is_open()) return;
    hotkeys_pause(); // иначе уже занятые клавиши не дойдут до окна
    static bool cls_reg = false;
    if (!cls_reg) {
        cls_reg = true;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = CLS_SETTINGS;
        RegisterClassExW(&wc);
    }
    if (st) {
        if (st->hwnd) { ShowWindow(st->hwnd, SW_SHOW); SetForegroundWindow(st->hwnd); }
        return;
    }

    Settings tmp;
    st = &tmp;
    tmp.dpi = monitor_dpi_at_cursor();
    auto fill = [&](int col, const std::vector<ParsedHotkey>& v) {
        for (int i = 0; i < MAXSLOTS; i++)
            tmp.list[col][i] = i < (int)v.size() ? v[i].raw : L"";
    };
    fill(0, g_cfg.hkFull);
    fill(1, g_cfg.hkRegion);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT rw = {0, 0, ssc(&tmp, 480), ssc(&tmp, 334)};
    AdjustWindowRectEx(&rw, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_TOPMOST | WS_EX_DLGMODALFRAME);
    int W = rw.right - rw.left, H = rw.bottom - rw.top;
    int x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - W) / 2;
    int y = mi.rcMonitor.top + ((mi.rcMonitor.bottom - mi.rcMonitor.top) - H) / 2;

    tmp.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, CLS_SETTINGS, LS(S_SET_TITLE),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H,
                               g_hwndMain, nullptr, inst, nullptr);
    if (!tmp.hwnd) { st = nullptr; return; }
    tmp.font = CreateFontW(-MulDiv(14, (int)tmp.dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(tmp.hwnd, WM_SETFONT, (WPARAM)tmp.font, TRUE);

    mk(&tmp, L"STATIC", LS(S_SET_FOLDER), SS_LEFT, 16, 12, 300, 20, 0, inst);
    tmp.edit = mk(&tmp, L"EDIT", g_cfg.shotsFolder.c_str(), ES_AUTOHSCROLL, 16, 34, 340, 26, IDC_EDIT_DIR, inst);
    mk(&tmp, L"BUTTON", LS(S_SET_BROWSE), BS_PUSHBUTTON, 368, 34, 96, 26, IDC_BROWSE, inst);

    mk(&tmp, L"STATIC", LS(S_SET_HK_FULL), SS_LEFT, 16, 74, 200, 20, 0, inst);
    mk(&tmp, L"STATIC", LS(S_SET_HK_R1), SS_LEFT, 250, 74, 200, 20, 0, inst);
    for (int col = 0; col < 2; col++) {
        int bx = col == 0 ? 16 : 250;
        int dx = col == 0 ? 172 : 406;
        for (int i = 0; i < MAXSLOTS; i++) {
            int sy = 100 + i * 30;
            tmp.btnKey[col][i] = mk(&tmp, L"BUTTON", L"", BS_PUSHBUTTON, bx, sy, 150, 26, IDC_KEY0 + col * 10 + i, inst);
            tmp.btnDel[col][i] = mk(&tmp, L"BUTTON", L"×", BS_PUSHBUTTON, dx, sy, 26, 26, IDC_DEL0 + col * 10 + i, inst);
        }
        tmp.btnAdd[col] = mk(&tmp, L"BUTTON", L"+", BS_PUSHBUTTON, bx, 224, 60, 26, IDC_ADD0 + col, inst);
    }

    mk(&tmp, L"STATIC", LS(S_SET_LANG), SS_LEFT, 16, 266, 140, 20, 0, inst);
    tmp.rdoRu = mk(&tmp, L"BUTTON", L"Русский", BS_AUTORADIOBUTTON | WS_GROUP, 160, 262, 100, 24, IDC_LANG_RU, inst);
    tmp.rdoEn = mk(&tmp, L"BUTTON", L"English", BS_AUTORADIOBUTTON, 268, 262, 100, 24, IDC_LANG_EN, inst);
    SendMessageW(g_lang == LANG_EN ? tmp.rdoEn : tmp.rdoRu, BM_SETCHECK, BST_CHECKED, 0);

    mk(&tmp, L"BUTTON", LS(S_SET_SAVE), BS_DEFPUSHBUTTON, 288, 294, 88, 28, IDC_SAVE, inst);
    mk(&tmp, L"BUTTON", LS(S_SET_CANCEL), BS_PUSHBUTTON, 384, 294, 80, 28, IDC_CANCEL, inst);
    settings_refresh(&tmp);

    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    ShowWindow(tmp.hwnd, SW_SHOW);
    SetForegroundWindow(tmp.hwnd);
    for (int i = 0; i < 6 && GetForegroundWindow() != tmp.hwnd; i++) {
        Sleep(50);
        SetForegroundWindow(tmp.hwnd);
    }

    MSG m;
    while (!tmp.done) {
        BOOL g = GetMessageW(&m, nullptr, 0, 0);
        if (g == 0) { tmp.done = true; break; }
        if (g == -1) break;
        if (!IsDialogMessageW(tmp.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DestroyWindow(tmp.hwnd);
    DeleteObject(tmp.font);
    st = nullptr;
    hotkeys_resume();
}

// ---------------- меню трея ----------------

static volatile bool g_menuTracking = false;
static volatile int g_pendingAction = -1;

bool tray_menu_open() { return g_menuTracking; }

void tray_menu_close_with_action(int id) {
    g_pendingAction = id;
    EndMenu(); // завершает модальный цикл TrackPopupMenu в этом же потоке
}

static void show_tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_OPEN_FOLDER, LS(S_MENU_FOLDER));
    AppendMenuW(m, MF_STRING, IDM_AUTOSTART, LS(S_MENU_AUTOSTART));
    CheckMenuItem(m, IDM_AUTOSTART, MF_BYCOMMAND | (autostart_is_on() ? MF_CHECKED : MF_UNCHECKED));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, LS(S_MENU_SETTINGS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    {
        std::wstring r1 = join_raw(g_cfg.hkFull);
        std::wstring r2 = join_raw(g_cfg.hkRegion);
        std::wstring s1 = std::wstring(LS(S_MENU_HK_FULL)) + L" (" + (r1.empty() ? L"-" : r1) + L")";
        std::wstring s2 = std::wstring(LS(S_MENU_HK_REGION)) + L" (" + (r2.empty() ? L"-" : r2) + L")";
        AppendMenuW(m, MF_STRING, IDM_HK_FULL, s1.c_str());
        AppendMenuW(m, MF_STRING, IDM_HK_REGION, s2.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, LS(S_MENU_EXIT));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // меню корректно закрывается по клику вне
    g_menuTracking = true;
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    g_menuTracking = false;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    // если во время меню нажали хоткей — выполняем действие сразу после закрытия
    int act = g_pendingAction;
    g_pendingAction = -1;
    if (act >= 0) hotkey_action(act);
}

void tray_on_notify(HWND hwnd, LPARAM lp) {
    WORD ev = LOWORD(lp); // NOTIFYICON_VERSION_4: сообщение в low-word
    switch (ev) {
        case WM_CONTEXTMENU:
            show_tray_menu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            open_shots_folder();
            break;
        default:
            break; // NIN_SELECT — клик левой кнопкой намеренно ничего не делает
    }
}

void tray_on_command(WORD id) {
    switch (id) {
        case IDM_OPEN_FOLDER:
            open_shots_folder();
            break;
        case IDM_SETTINGS:
            settings_open();
            break;
        case IDM_AUTOSTART:
            autostart_set(!autostart_is_on());
            break;
        case IDM_HK_FULL:
            hotkey_edit_dialog(1);
            break;
        case IDM_HK_REGION:
            hotkey_edit_dialog(2);
            break;
        case IDM_EXIT:
            if (g_hwndMain) PostMessageW(g_hwndMain, WM_APP_EXIT, 0, 0);
            break;
    }
}
