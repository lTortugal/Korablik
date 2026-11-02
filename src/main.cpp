// Точка входа, скрытое окно, хоткеи, хук PrintScreen, цикл сообщений
#include "app.h"
#include <cstdlib>
#include <cwchar>

HWND g_hwndMain = nullptr;
bool g_quitRequested = false;
static HANDLE g_mutex = nullptr;
static HHOOK g_kbdHook = nullptr;
static bool g_hotkeysPaused = false;

std::wstring winerr_text(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&msg, 0, nullptr);
    std::wstring s;
    if (n && msg) {
        s.assign(msg, n);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    }
    if (msg) LocalFree(msg);
    if (s.empty()) {
        wchar_t b[32];
        swprintf(b, 32, L"код %lu", code);
        s = b;
    }
    return s;
}

// освобождение рабочего набора: после снимка возвращаем памяти столько, сколько взяли
void trim_working_set() {
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
}

static void do_full_shot() {
    Shot s;
    RECT rc;
    UINT dpi;
    std::wstring err;
    if (!capture_cursor_monitor(s, rc, dpi, err)) {
        tray_notify(LS(S_APP), err.c_str());
        return;
    }
    export_shot(s, EXP_CLIP_FILE);
    s.free();
    trim_working_set();
}

static void do_region_shot() {
    Shot s;
    RECT rc;
    UINT dpi;
    std::wstring err;
    if (!capture_cursor_monitor(s, rc, dpi, err)) {
        tray_notify(LS(S_APP), err.c_str());
        return;
    }
    overlay_run(std::move(s), rc, dpi); // буфер освобождает сам оверлей
    trim_working_set();
}

// id 1 = полный монитор, 2/3 = область
void hotkey_action(int id) {
    // меню трея перехватывает мышь своим модальным циклом — закрываем его и действуем после
    if (tray_menu_open()) { tray_menu_close_with_action(id); return; }
    if (overlay_is_open()) return; // защита от повторного входа
    if (tray_hk_dialog_open()) return; // идёт настройка хоткея
    if (id == HKID_FULL) do_full_shot();
    else do_region_shot();
}

// Low-level хук нужен для PrintScreen: иначе Windows открывает системный Snipping Tool
static int snapshot_action() { // куда ведёт PrintScreen по текущему конфигу
    for (auto& hk : g_cfg.hkFull)
        if (hk.vk == VK_SNAPSHOT) return HKID_FULL;
    for (auto& hk : g_cfg.hkRegion)
        if (hk.vk == VK_SNAPSHOT) return HKID_REGION1;
    return 0;
}

static LRESULT CALLBACK kbd_ll(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        PKBDLLHOOKSTRUCT k = (PKBDLLHOOKSTRUCT)lp;
        if (k->vkCode == VK_SNAPSHOT && !tray_hk_dialog_open()) {
            int act = snapshot_action();
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (act && now - last > 700) { // гашение автоповтора
                last = now;
                if (g_hwndMain) PostMessageW(g_hwndMain, WM_APP_ACTION, act, 0);
            }
            return 1; // подавляем системное поведение PrtScn
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_APP_ACTION:
            hotkey_action((int)w);
            return 0;
        case WM_HOTKEY:
            // id 1..8 = снимок монитора, 21..28 = область
            hotkey_action(w < 21 ? HKID_FULL : HKID_REGION1);
            return 0;
        case WM_APP_TRAY:
            tray_on_notify(h, l);
            return 0;
        case WM_COMMAND:
            tray_on_command(LOWORD(w));
            return 0;
        case WM_APP_EXIT:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_quitRequested = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void register_hotkeys() {
    int id = 1;
    for (auto& hk : g_cfg.hkFull) {
        if (hk.vk == VK_SNAPSHOT) continue; // PrintScreen обрабатывает хук
        if (id > 8) break;
        // MOD_NOREPEAT: зажатая клавиша не вызывает серию снимков
        if (!RegisterHotKey(g_hwndMain, id, hk.mods | MOD_NOREPEAT, hk.vk)) {
            std::wstring t = L"Не удалось зарегистрировать " + hk.raw +
                             L" — вероятно, клавиша занята другим приложением";
            tray_notify(LS(S_APP), t.c_str());
        }
        id++;
    }
    id = 21;
    for (auto& hk : g_cfg.hkRegion) {
        if (hk.vk == VK_SNAPSHOT) continue;
        if (id > 28) break;
        if (!RegisterHotKey(g_hwndMain, id, hk.mods | MOD_NOREPEAT, hk.vk)) {
            std::wstring t = L"Не удалось зарегистрировать " + hk.raw +
                             L" — вероятно, клавиша занята другим приложением";
            tray_notify(LS(S_APP), t.c_str());
        }
        id++;
    }
}

static void set_snapshot_hook(bool need) {
    if (need && !g_kbdHook) {
        g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_ll, GetModuleHandleW(nullptr), 0);
        if (!g_kbdHook)
            tray_notify(LS(S_APP), (L"Не удалось перехватить PrintScreen: " + winerr_text(GetLastError())).c_str());
    } else if (!need && g_kbdHook) {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
}

// применяется после изменения [hotkeys] в settings.ini через меню трея
void hotkeys_apply() {
    for (int i = 1; i <= 28; i++) UnregisterHotKey(g_hwndMain, i);
    if (g_hotkeysPaused) return; // окно настройки открыто — зарегистрируем при resume
    register_hotkeys();
    set_snapshot_hook(g_cfg.useSnapshotHook);
}

// Пока открыто окно захвата клавиши, глобальные хоткеи мешают: система съедает
// нажатие раньше, чем оно дойдёт до окна (например F18 уже занят как наш хоткей).
void hotkeys_pause() {
    if (g_hotkeysPaused) return;
    g_hotkeysPaused = true;
    for (int i = 1; i <= 28; i++) UnregisterHotKey(g_hwndMain, i);
    if (g_kbdHook) {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
}

void hotkeys_resume() {
    if (!g_hotkeysPaused) return;
    g_hotkeysPaused = false;
    register_hotkeys();
    set_snapshot_hook(g_cfg.useSnapshotHook);
}

int main() {
    // Per-Monitor DPI v2 из кода на случай, если манифест не применится
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (auto fn = (BOOL(APIENTRY*)(HANDLE))GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
        fn((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    else
        SetProcessDPIAware();

    // только один экземпляр
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Korablik_SingleInstance_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    config_load();
    config_create_default_ini();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WinShotHidden";
    RegisterClassExW(&wc);
    overlay_register_classes(inst);

    g_hwndMain = CreateWindowExW(0, wc.lpszClassName, L"Корабль", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_hwndMain) return 1;

    register_hotkeys();
    set_snapshot_hook(g_cfg.useSnapshotHook);

    tray_add(g_hwndMain);
    trim_working_set(); // стартовые страницы CRT сразу не нужны

    // служебный флаг для автотестов: --hktest-full / --hktest-region / --settings
    if (wcsstr(GetCommandLineW(), L"--hktest-full") != nullptr) hotkey_edit_dialog(1);
    else if (wcsstr(GetCommandLineW(), L"--hktest-region") != nullptr) hotkey_edit_dialog(2);
    else if (wcsstr(GetCommandLineW(), L"--settings") != nullptr) settings_open();

    MSG msg;
    while (!g_quitRequested) {
        BOOL g = GetMessageW(&msg, nullptr, 0, 0);
        if (g == 0 || g == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    tray_remove();
    if (g_kbdHook) UnhookWindowsHookEx(g_kbdHook);
    for (int i = 1; i <= 28; i++) UnregisterHotKey(g_hwndMain, i);
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
