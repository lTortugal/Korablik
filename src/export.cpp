// Экспорт результата: PNG-кодировщик (stb_image_write), буфер обмена,
// папка со скриншотами и нумерация файлов 1.png, 2.png, ...
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "app.h"
#include "../vendor/stb_image_write.h"
#include <cstdlib>
#include <cwchar>

// ---------- PNG в память ----------
static void write_png_chunk(void* ctx, void* data, int size) {
    auto* v = (std::vector<unsigned char>*)ctx;
    size_t n = v->size();
    v->resize(n + (size_t)size);
    memcpy(v->data() + n, data, (size_t)size);
}

// BGRA -> RGBA меняем на месте (без второго полноэкранного буфера), кодируем, возвращаем как было
static bool encode_png(Shot& s, std::vector<unsigned char>& png) {
    size_t count = (size_t)s.w * s.h;
    for (size_t i = 0; i < count; i++) {
        unsigned char t = s.px[i].rgbRed;
        s.px[i].rgbRed = s.px[i].rgbBlue;
        s.px[i].rgbBlue = t;
    }
    png.clear();
    png.reserve(count * 2);
    int res = stbi_write_png_to_func(&write_png_chunk, &png, s.w, s.h, 4, s.px, s.w * 4);
    for (size_t i = 0; i < count; i++) {
        unsigned char t = s.px[i].rgbRed;
        s.px[i].rgbRed = s.px[i].rgbBlue;
        s.px[i].rgbBlue = t;
    }
    if (!res) {
        png.clear();
        return false;
    }
    return true;
}

// ---------- буфер обмена ----------
static HGLOBAL build_dib_handle(const RGBQUAD* px, int w, int h) {
    size_t imgSize = (size_t)w * h * 4;
    size_t total = sizeof(BITMAPINFOHEADER) + imgSize;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!g) return nullptr;
    auto* hdr = (BITMAPINFOHEADER*)GlobalLock(g);
    if (!hdr) { GlobalFree(g); return nullptr; }
    memset(hdr, 0, sizeof(*hdr));
    hdr->biSize = sizeof(BITMAPINFOHEADER);
    hdr->biWidth = w;
    hdr->biHeight = -h; // top-down, так читают Photoshop/Word/мессенджеры
    hdr->biPlanes = 1;
    hdr->biBitCount = 32;
    hdr->biCompression = BI_RGB;
    hdr->biSizeImage = (DWORD)imgSize;
    memcpy((unsigned char*)(hdr + 1), px, imgSize);
    GlobalUnlock(g);
    return g;
}

static bool put_to_clipboard(const Shot& s, const std::vector<unsigned char>& png, std::wstring& err) {
    // OpenClipboard может быть занят — повторяем попытки с короткой паузой
    bool opened = false;
    for (int i = 0; i < 10 && !opened; i++) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(30);
    }
    if (!opened) {
        err = L"Буфер обмена занят другим приложением";
        return false;
    }
    bool ok = false;
    do {
        if (!EmptyClipboard()) break;
        HGLOBAL gdib = build_dib_handle(s.px, s.w, s.h);
        if (!gdib) break;
        if (!SetClipboardData(CF_DIB, gdib)) {
            GlobalFree(gdib);
            break;
        }
        // дополнительный формат "PNG" — для приложений, которые его поддерживают
        if (!png.empty()) {
            UINT fmtPng = RegisterClipboardFormatW(L"PNG");
            if (fmtPng) {
                HGLOBAL gpng = GlobalAlloc(GMEM_MOVEABLE, png.size());
                if (gpng) {
                    void* p = GlobalLock(gpng);
                    if (p) {
                        memcpy(p, png.data(), png.size());
                        GlobalUnlock(gpng);
                        if (!SetClipboardData(fmtPng, gpng)) GlobalFree(gpng);
                    } else {
                        GlobalFree(gpng);
                    }
                }
            }
        }
        ok = true;
    } while (0);
    if (!ok) err = L"SetClipboardData не удалась: " + winerr_text(GetLastError());
    CloseClipboard();
    return ok;
}

// ---------- папка и нумерация ----------
// SHCreateDirectoryExW для уже существующей папки возвращает ERROR_ALREADY_EXISTS — это тоже успех
static bool ensure_dir(const std::wstring& path) {
    LONG r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS;
}

// создаём папку (включая промежуточные); при недоступности — откат на %USERPROFILE%\Documents\Screenshots
static std::wstring resolve_folder() {
    if (!g_cfg.shotsFolder.empty() && ensure_dir(g_cfg.shotsFolder))
        return g_cfg.shotsFolder;

    wchar_t docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        std::wstring fb = std::wstring(docs) + L"\\Screenshots";
        if (ensure_dir(fb)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                tray_notify(LS(S_APP),
                    (L"Папка " + g_cfg.shotsFolder + L" недоступна, сохраняю в " + fb).c_str());
            }
            return fb;
        }
    }
    tray_notify(LS(S_APP), (L"Не удалось создать папку: " + winerr_text(GetLastError())).c_str());
    return std::wstring();
}

static bool name_is_numbered_png(const wchar_t* name, unsigned int& value) {
    size_t len = wcslen(name);
    if (len < 5) return false;
    if (_wcsicmp(name + len - 4, L".png") != 0) return false;
    size_t digits = len - 4;
    for (size_t i = 0; i < digits; i++)
        if (name[i] < L'0' || name[i] > L'9') return false;
    // без ведущих нулей-мусора и без переполнения
    if (digits > 8 || (digits > 1 && name[0] == L'0')) return false;
    unsigned long v = wcstoul(name, nullptr, 10);
    if (v == 0) return false;
    value = (unsigned int)v;
    return true;
}

// наименьшее свободное число N: 1.png, 2.png, ... (после удаления 3 следующий — снова 3.png)
static bool next_numbered_file(const std::wstring& dir, std::wstring& outPath) {
    unsigned int maxSeen = 0;
    std::vector<unsigned char> used; // побитовая карта занятных номеров
    auto ensure_bit = [&](unsigned int n) {
        size_t byte = (n - 1) / 8;
        if (byte >= used.size()) used.resize(byte + 1, 0);
    };
    auto set_bit = [&](unsigned int n) {
        ensure_bit(n);
        used[(n - 1) / 8] |= (unsigned char)(1 << ((n - 1) % 8));
    };
    auto get_bit = [&](unsigned int n) -> bool {
        if (n == 0) return true;
        size_t byte = (n - 1) / 8;
        if (byte >= used.size()) return false;
        return (used[byte] >> ((n - 1) % 8)) & 1;
    };

    WIN32_FIND_DATAW fd = {};
    std::wstring pat = dir + L"\\*.png";
    HANDLE hFind = FindFirstFileW(pat.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            unsigned int v = 0;
            if (name_is_numbered_png(fd.cFileName, v)) {
                set_bit(v);
                if (v > maxSeen) maxSeen = v;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    } else if (GetLastError() != ERROR_NO_MORE_FILES && GetLastError() != ERROR_FILE_NOT_FOUND) {
        tray_notify(LS(S_APP), (L"Ошибка чтения папки: " + winerr_text(GetLastError())).c_str());
        return false;
    }

    // CREATE_NEW закрывает гонку: если файл появился между поиском и созданием — пробуем следующий номер
    for (unsigned int n = 1; n <= maxSeen + 1; n++) {
        if (get_bit(n)) continue;
        wchar_t num[16];
        swprintf(num, 16, L"%u", n);
        std::wstring path = dir + L"\\" + num + L".png";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h); // не держать дескриптор: иначе write_png_file упрётся в sharing violation
            outPath = path;
            return true;
        }
        DWORD le = GetLastError();
        if (le == ERROR_FILE_EXISTS || le == ERROR_ALREADY_EXISTS) {
            set_bit(n);
            continue;
        }
        tray_notify(LS(S_APP), (L"Не удалось создать файл: " + winerr_text(le)).c_str());
        return false;
    }
    return false;
}

static bool write_png_file(const std::wstring& path, const std::vector<unsigned char>& png) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        tray_notify(LS(S_APP), (L"Не открылся файл: " + winerr_text(GetLastError())).c_str());
        return false;
    }
    size_t off = 0;
    bool ok = true;
    while (off < png.size()) {
        DWORD wr = 0;
        DWORD chunk = (DWORD)(png.size() - off > 0x4000000 ? 0x4000000 : png.size() - off);
        if (!WriteFile(h, png.data() + off, chunk, &wr, nullptr) || wr == 0) {
            ok = false;
            break;
        }
        off += wr;
    }
    CloseHandle(h);
    if (!ok) tray_notify(LS(S_APP), (L"Ошибка записи файла: " + winerr_text(GetLastError())).c_str());
    return ok;
}

// ---------- общая точка выхода ----------
bool export_shot(Shot& s, int mode) {
    // GDI при рисовании кисти обнуляет альфа-байт в DIB; без этого PNG с штрихами
    // имеет alpha=0 и просмотрщики (Telegram и др.) показывают штрихи белыми
    for (size_t i = 0, n = (size_t)s.w * s.h; i < n; i++) s.px[i].rgbReserved = 255;

    bool needFile = (mode == EXP_CLIP_FILE || mode == EXP_FILE_ONLY);
    bool needClip = (mode == EXP_CLIP_FILE || mode == EXP_CLIP_ONLY);

    std::vector<unsigned char> png;
    bool havePng = encode_png(s, png);
    if (needFile && !havePng) {
        tray_notify(LS(S_APP), L"Не удалось закодировать PNG");
        return false;
    }

    bool clipOk = true, fileOk = true;
    if (needClip) {
        std::wstring err;
        clipOk = put_to_clipboard(s, png, err);
        if (!clipOk) tray_notify(LS(S_APP), err.c_str());
    }
    if (needFile) {
        std::wstring dir = resolve_folder();
        std::wstring path;
        fileOk = !dir.empty() && next_numbered_file(dir, path) && write_png_file(path, png);
    }
    return clipOk && fileOk;
}

void open_shots_folder() {
    std::wstring dir = resolve_folder();
    if (dir.empty()) return;
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
