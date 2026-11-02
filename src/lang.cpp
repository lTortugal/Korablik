// Таблицы строк RU/EN
#include "app.h"

Lang g_lang = LANG_RU;

void lang_set(const wchar_t* value) {
    g_lang = (value && _wcsicmp(value, L"en") == 0) ? LANG_EN : LANG_RU;
}

static const wchar_t* ru[S_COUNT] = {
    L"Кораблик",
    L"Открыть папку со скриншотами",
    L"Запускать с Windows",
    L"⚙ Настройки…",
    L"Хоткей весь экран",
    L"Хоткей область",
    L"Выход",
    L"Хоткей: снимок монитора",
    L"Хоткей: выделение области",
    L"Хоткей области: второй",
    L"сейчас: ",
    L"Нажми новую клавишу",
    L"Esc или правая кнопка — отмена",
    L"нажми клавишу…",
    L"Настройки Кораблика",
    L"Папка для скриншотов:",
    L"Обзор…",
    L"Хоткей весь экран:",
    L"Хоткей область:",
    L"Язык / Language:",
    L"Сохранить",
    L"Отмена",
    L"Настройки сохранены",
    L"Папка для скриншотов",
    L"Хоткей снимка монитора: ",
    L"Хоткей области: ",
    L"Хоткей области (второй): ",
    L"Эту клавишу назначить нельзя — поддерживаются F1–F24, PrintScreen, буквы, цифры, Space, Enter",
    L"Esc — отмена",
};

static const wchar_t* en[S_COUNT] = {
    L"Korablik",
    L"Open screenshots folder",
    L"Run at Windows startup",
    L"⚙ Settings…",
    L"Full-screen hotkey",
    L"Region hotkey",
    L"Exit",
    L"Hotkey: full monitor shot",
    L"Hotkey: region capture",
    L"Hotkey: region (second)",
    L"current: ",
    L"Press a new key",
    L"Esc or right-click — cancel",
    L"press a key…",
    L"Korablik settings",
    L"Screenshots folder:",
    L"Browse…",
    L"Full-screen hotkey:",
    L"Region hotkey:",
    L"Language / Язык:",
    L"Save",
    L"Cancel",
    L"Settings saved",
    L"Screenshots folder",
    L"Full monitor hotkey: ",
    L"Region hotkey 1: ",
    L"Region hotkey 2: ",
    L"This key cannot be assigned — supported: F1–F24, PrintScreen, letters, digits, Space, Enter",
    L"Esc — cancel",
};

const wchar_t* LS(StrId id) {
    if (id < 0 || id >= S_COUNT) return L"?";
    return g_lang == LANG_EN ? en[id] : ru[id];
}
