@echo off
setlocal

set "_DST=%~dp0Version.h"
set "_TMP=%~dp0Version.h.tmp"
set "_LOCK=%~dp0Version.h.lock"

rem 並列ビルドで同時に実行されることがあるため、ロック用ディレクトリで排他制御する
set /a _TRY=0
:lock
2>nul mkdir "%_LOCK%" && goto locked
set /a _TRY+=1
if %_TRY% geq 120 (
    rem 異常終了で残ったロックとみなして削除し、取り直す
    echo Version.bat: lock timeout, removing stale lock "%_LOCK%"
    rmdir "%_LOCK%" >nul 2>&1
    set /a _TRY=0
)
ping -n 2 127.0.0.1 >nul
goto lock
:locked

for /f "usebackq delims=" %%A in (`git describe --tags`) do set "VER_FULL=%%A"
for /f "usebackq delims= tokens=*" %%A in (`git describe "--abbrev=0" --tags`) do set "VER_TAG=%%A"

> "%_TMP%" (
echo #define AMATSUKAZE_VERSION "%VER_FULL%"
echo #define AMATSUKAZE_PRODUCTVERSION %VER_TAG:.=,%
)

set "_RET=0"
if not exist "%_DST%" (
    move /y "%_TMP%" "%_DST%" >nul || set "_RET=1"
    goto unlock
)

fc /b "%_TMP%" "%_DST%" >nul
if %ERRORLEVEL% == 0 (
    del "%_TMP%" >nul 2>&1
) else (
    move /y "%_TMP%" "%_DST%" >nul || set "_RET=1"
)

:unlock
rmdir "%_LOCK%" >nul 2>&1
endlocal & exit /b %_RET%
