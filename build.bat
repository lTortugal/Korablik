@echo off
chcp 65001 >nul
setlocal
rem =====================================================================
rem  Сборка Корабль: один exe, без внешних зависимостей (статический CRT)
rem  Нужны: Visual Studio 2022 (рабочие нагрузки C++) и Windows SDK 10
rem =====================================================================

set "VSVARS="
for %%P in ("D:\Programs\Visual Studio\Main\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat") do if exist %%P set "VSVARS=%%~P"

if "%VSVARS%"=="" goto :novs
call "%VSVARS%" >nul
if errorlevel 1 goto :vvarsfail

if not exist build mkdir build
pushd src
rc /nologo /fo ..\build\app.res app.rc
popd
if errorlevel 1 goto :resfail

rem /O2 /GL /LTCG — whole-program оптимизация, /MT — статический CRT (никаких vcruntime DLL),
rem /Gy /OPT:REF /OPT:ICF — вырезание неиспользуемого кода, /utf-8 — кириллица в исходниках
cl /nologo /utf-8 /std:c++17 /O2 /Oi /Ot /GL /MT /Gy /EHsc /W3 /DUNICODE /D_UNICODE src\main.cpp src\lang.cpp src\config.cpp src\capture.cpp src\export.cpp src\overlay.cpp src\tray.cpp /Fo:build\ /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OPT:REF /OPT:ICF /LTCG build\app.res user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib /OUT:build\Korablik.exe
if errorlevel 1 goto :compfail

echo.
echo Готово: build\Korablik.exe
dir build\Korablik.exe
exit /b 0

:novs
echo Не найден vcvars64.bat. Установи Visual Studio 2022 с рабочей нагрузкой "Разработка классических приложений на C++" и Windows SDK 10.
exit /b 1
:vvarsfail
echo Не удалось инициализировать окружение MSVC.
exit /b 1
:resfail
echo Ошибка компиляции ресурсов (манифеста).
exit /b 1
:compfail
echo Сборка не удалась.
exit /b 1
