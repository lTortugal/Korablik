// Чтение/запись settings.ini рядом с exe и разбор хоткеев
#include "app.h"
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string_view>

Config g_cfg;

static std::wstring ini_get_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(1024);
    for (;;) {
        DWORD n = GetPrivateProfileStringW(sec, key, def, buf.data(), (DWORD)buf.size(), g_cfg.iniPath.c_str());
        if (n + 1 < buf.size()) {
            buf.resize(n + 1);
            return std::wstring(buf.data());
        }
        buf.resize(buf.size() * 2);
    }
}

static BOOL ini_set_str(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    return WritePrivateProfileStringW(sec, key, val.c_str(), g_cfg.iniPath.c_str());
}

// "CTRL+ALT+F20" -> vk/mods; "F18", "PRINTSCREEN", "PRTSC", "SNAPSHOT", "A".."Z", "0".."9"
static ParsedHotkey parse_hotkey(const std::wstring& raw) {
    ParsedHotkey hk;
    hk.raw = raw;
    std::wstring key;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t plus = raw.find(L'+', pos);
        std::wstring tok = raw.substr(pos, plus == std::wstring::npos ? std::wstring::npos : plus - pos);
        // trim + upper
        size_t a = tok.find_first_not_of(L" \t");
        size_t b = tok.find_last_not_of(L" \t");
        if (a != std::wstring::npos) tok = tok.substr(a, b - a + 1); else tok.clear();
        for (auto& c : tok) c = (wchar_t)towupper(c);
        if (tok == L"CTRL" || tok == L"CONTROL") hk.mods |= MOD_CONTROL;
        else if (tok == L"SHIFT") hk.mods |= MOD_SHIFT;
        else if (tok == L"ALT") hk.mods |= MOD_ALT;
        else if (tok == L"WIN") hk.mods |= MOD_WIN;
        else if (!tok.empty()) key = tok;
        if (plus == std::wstring::npos) break;
        pos = plus + 1;
    }
    if (key.empty()) return hk;

    if (key == L"PRINTSCREEN" || key == L"PRTSC" || key == L"SNAPSHOT" || key == L"PRTSCRN") {
        hk.vk = VK_SNAPSHOT;
    } else if (key == L"SPACE") {
        hk.vk = VK_SPACE;
    } else if (key == L"ENTER") {
        hk.vk = VK_RETURN;
    } else if (key[0] == L'F' && key.size() >= 2) {
        int n = _wtoi(key.c_str() + 1);
        if (n >= 1 && n <= 24) hk.vk = (UINT)(VK_F1 + (n - 1)); // F13..F24 = 0x7C..0x87
    } else if (key.size() == 1 && iswalnum(key[0])) {
        hk.vk = (UINT)key[0];
    } else {
        return hk;
    }
    hk.valid = true;
    return hk;
}

// "F18, CTRL+ALT+F20" -> список клавиш
static std::vector<ParsedHotkey> parse_list(const std::wstring& raw) {
    std::vector<ParsedHotkey> out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t comma = raw.find(L',', pos);
        std::wstring tok = raw.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        size_t a = tok.find_first_not_of(L" \t");
        size_t b = tok.find_last_not_of(L" \t");
        if (a != std::wstring::npos) tok = tok.substr(a, b - a + 1); else tok.clear();
        if (!tok.empty()) {
            ParsedHotkey hk = parse_hotkey(tok);
            if (hk.valid) out.push_back(hk);
        }
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return out;
}

static bool list_has_snapshot(const std::vector<ParsedHotkey>& v) {
    for (auto& hk : v)
        if (hk.vk == VK_SNAPSHOT) return true;
    return false;
}

// перечитать секцию [hotkeys]
static void config_read_hotkeys() {
    g_cfg.hkFull = parse_list(ini_get_str(L"hotkeys", L"full_monitor", L"F18"));
    g_cfg.hkRegion = parse_list(ini_get_str(L"hotkeys", L"region", L"PRINTSCREEN, F19"));
    // легаси-формат: второй слот области отдельным ключом
    std::vector<ParsedHotkey> legacy = parse_list(ini_get_str(L"hotkeys", L"region_secondary", L""));
    for (auto& hk : legacy) g_cfg.hkRegion.push_back(hk);
    g_cfg.useSnapshotHook = list_has_snapshot(g_cfg.hkFull) || list_has_snapshot(g_cfg.hkRegion);
}

// Автозапуск: задача планировщика с наивысшими правами (ONLOGON).
// Простой запуск через HKCU\Run не подходит: приложение требует админ-права,
// и Run-ключ каждый раз показывал бы UAC. Задача планировщика стартует молча.
static BOOL run_schtasks(std::wstring_view args, DWORD* exit_code) {
    std::wstring cmd = L"schtasks.exe";
    cmd += args;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code) *exit_code = ec;
    return TRUE;
}

bool autostart_is_on() {
    DWORD ec = 1;
    if (!run_schtasks(L" /Query /TN Korablik", &ec)) return false;
    return ec == 0;
}

void autostart_set(bool on) {
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exePath, (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
    if (on) {
        // /TR принимает команду в кавычках; путь с пробелами экранируем двойными кавычками
        std::wstring args = L" /Create /F /SC ONLOGON /TN Korablik /RL HIGHEST /TR \"\\\"";
        args += exePath;
        args += L"\\\"\"";
        if (!run_schtasks(args, nullptr)) {
            tray_notify(L"Корабль", L"Не удалось создать задачу автозапуска");
            return;
        }
    } else {
        run_schtasks(L" /Delete /TN Korablik /F", nullptr);
    }
    g_cfg.autostart = on;
    ini_set_str(L"general", L"autostart", on ? L"1" : L"0");
}

void config_load() {
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exePath, (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
    std::wstring dir = exePath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    g_cfg.iniPath = dir + L"\\settings.ini";

    g_cfg.shotsFolder = ini_get_str(L"general", L"screens_folder", L"D:\\Документы\\Screenshots");
    // если путь в ini задан без слэсов-экранирования — прибрать
    if (g_cfg.shotsFolder.empty()) g_cfg.shotsFolder = L"D:\\Документы\\Screenshots";

    config_read_hotkeys();

    std::wstring aut = ini_get_str(L"general", L"autostart", L"0");
    g_cfg.autostart = autostart_is_on(); // состояние берём из реестра/задач, ini — только подсказка
    (void)aut;

    std::wstring lang = ini_get_str(L"general", L"language", L"ru");
    lang_set(lang.c_str());
}

// перечитать только хоткеи (после изменения через меню трея)
void config_reload_hotkeys() {
    config_read_hotkeys();
}

void config_create_default_ini() {
    // если файла нет — создаём с комментариями, UTF-16LE с BOM (чтобы кириллица работала везде)
    const wchar_t* text =
        L"; Кораблик - настройки\r\n"
        L"; hotkeys: F1..F24, PRINTSCREEN, SPACE, ENTER, A-Z, 0-9; модификаторы CTRL/SHIFT/ALT/WIN через '+'\r\n"
        L"; на одно действие можно назначить несколько клавиш через запятую\r\n"
        L"; менять хоткеи можно и из меню трея (правая кнопка по иконке)\r\n"
        L"[general]\r\n"
        L"screens_folder=D:\\Документы\\Screenshots\r\n"
        L"language=ru\r\n"
        L"autostart=0\r\n"
        L"[hotkeys]\r\n"
        L"full_monitor=F18\r\n"
        L"region=PRINTSCREEN, F19\r\n";

    HANDLE h = CreateFileW(g_cfg.iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return; // файл уже есть
    DWORD written = 0;
    const unsigned short bom = 0xFEFF;
    WriteFile(h, &bom, 2, &written, nullptr);
    WriteFile(h, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}
