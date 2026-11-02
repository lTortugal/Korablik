// Общие объявления «Корабль» — лёгкая утилита для скриншотов в трее
#pragma once
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STRICT
#include <windows.h>
#include "lang.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

// ---- сообщения между окнами ----
#define WM_APP_TRAY   (WM_APP + 1) // уведомления из трея
#define WM_APP_ACTION (WM_APP + 2) // выполнение действия из хука PrintScreen
#define WM_APP_EXIT   (WM_APP + 3) // выход из меню трея

// ---- идентификаторы меню трея ----
#define IDM_OPEN_FOLDER 100
#define IDM_AUTOSTART   101
#define IDM_EXIT        102
#define IDM_HK_FULL     103
#define IDM_HK_REGION   104
#define IDM_HK_REGION2  105
#define IDM_SETTINGS    106

// ---- идентификаторы хоткеев для RegisterHotKey ----
#define HKID_FULL    1
#define HKID_REGION1 2
#define HKID_REGION2 3

// tray.cpp
void tray_add(HWND owner);
void tray_remove();
void tray_notify(const wchar_t* title, const wchar_t* text);
void tray_on_notify(HWND hwnd, LPARAM lp);  // WM_APP_TRAY из скрытого окна
void tray_on_command(WORD id);              // команды контекстного меню
void hotkey_edit_dialog(int which);         // 1 = full_monitor, 2 = region, 3 = region_secondary
bool tray_hk_dialog_open();                 // диалог перехвата клавиши открыт
void settings_open();                       // окно настроек (папка + хоткеи)
bool tray_menu_open();                      // всплывающее меню трея сейчас открыто
void tray_menu_close_with_action(int id);   // закрыть меню и выполнить действие после

// config.cpp (автозапуск: задача планировщика с наивысшими правами)
bool autostart_is_on();
void autostart_set(bool on);

// текст системной ошибки по коду GetLastError()
std::wstring winerr_text(DWORD code);

// урезание рабочего набора до минимума (в Idle)
void trim_working_set();

// ---- снимок монитора ----
struct Shot {
    RGBQUAD* px = nullptr; // строки подряд сверху вниз, BGRA
    int w = 0, h = 0;
    void free() { delete[] px; px = nullptr; w = h = 0; }
};

// capture.cpp: снимает монитор, на котором стоит курсор
// rcMon — физические пиксели монитора, dpi — реальный DPI этого монитора
bool capture_cursor_monitor(Shot& out, RECT& rcMon, UINT& dpi, std::wstring& err);
UINT monitor_dpi_at_cursor();

// export.cpp: сохранить снимок; mode = 1 буфер+файл, 2 только буфер, 3 только файл
enum ExportMode { EXP_CLIP_FILE = 1, EXP_CLIP_ONLY = 2, EXP_FILE_ONLY = 3 };
bool export_shot(Shot& s, int mode); // ошибки показывает уведомлением из трея
void open_shots_folder();           // открыть текущую папку в проводнике

// overlay.cpp
void overlay_register_classes(HINSTANCE inst);
bool overlay_is_open();
void overlay_run(Shot&& shot, const RECT& monRc, UINT dpi); // блокирующий модальный цикл

// config.cpp
struct ParsedHotkey {
    bool valid = false;
    UINT vk = 0;
    UINT mods = 0;
    std::wstring raw;
};

struct Config {
    std::wstring iniPath;
    std::wstring shotsFolder = L"D:\\Документы\\Screenshots";
    std::vector<ParsedHotkey> hkFull;   // список клавиш для снимка монитора
    std::vector<ParsedHotkey> hkRegion; // список клавиш для выделения области
    bool useSnapshotHook = false; // True, если хотя бы одна клавиша — PrintScreen
    bool autostart = false;       // текущее состояние автозапуска
};
extern Config g_cfg;
void config_load();
void config_reload_hotkeys(); // перечитать секцию [hotkeys]
void config_create_default_ini();

// main.cpp: перерегистрировать хоткеи после изменения конфига
void hotkeys_apply();
// main.cpp: пауза/возобновление глобальных хоткеев (на время захвата клавиши в настройках)
void hotkeys_pause();
void hotkeys_resume();
// main.cpp: выполнить действие по хоткею (id = HKID_*)
void hotkey_action(int id);
