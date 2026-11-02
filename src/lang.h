// Двухъязычные строки интерфейса (RU/EN), выбор в settings.ini: language=ru|en
#pragma once

enum Lang { LANG_RU = 0, LANG_EN = 1 };
extern Lang g_lang;
void lang_set(const wchar_t* value); // "ru" / "en"

enum StrId {
    S_APP = 0,          // Кораблик / Korabl
    S_MENU_FOLDER,      // Открыть папку со скриншотами
    S_MENU_AUTOSTART,   // Запускать с Windows
    S_MENU_SETTINGS,    // ⚙ Настройки…
    S_MENU_HK_FULL,     // Хоткей весь экран (…)
    S_MENU_HK_REGION,   // Хоткей область (…)
    S_MENU_EXIT,        // Выход
    S_DLG_FULL,         // Хоткей: снимок монитора
    S_DLG_REGION,       // Хоткей: выделение области
    S_DLG_REGION2,      // Хоткей области: второй
    S_DLG_NOW,          // сейчас: / current:
    S_DLG_PRESS,        // Нажми новую клавишу
    S_DLG_ESC,          // Esc или правая кнопка — отмена
    S_DLG_PRESSING,     // нажми клавишу…
    S_SET_TITLE,        // Настройки Кораблика
    S_SET_FOLDER,       // Папка для скриншотов:
    S_SET_BROWSE,       // Обзор…
    S_SET_HK_FULL,      // Хоткей весь экран:
    S_SET_HK_R1,        // Хоткей область:
    S_SET_LANG,         // Язык / Language:
    S_SET_SAVE,         // Сохранить
    S_SET_CANCEL,       // Отмена
    S_SET_SAVED,        // Настройки сохранены
    S_BROWSE_TITLE,     // Папка для скриншотов
    S_HK_SET_FULL,      // Хоткей снимка монитора:
    S_HK_SET_R1,        // Хоткей области:
    S_HK_SET_R2,        // Хоткей области (второй):
    S_ERR_BAD_KEY,      // клавишу назначить нельзя…
    S_OV_ESC,           // Esc — отмена / Esc — cancel
    S_COUNT
};

const wchar_t* LS(StrId id);
