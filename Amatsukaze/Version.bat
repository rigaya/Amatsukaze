@echo off
setlocal

set "_DST=%~dp0Version.h"
set "_TMP=%~dp0Version.h.tmp"

for /f "usebackq delims=" %%A in (`git describe --tags`) do set "VER_FULL=%%A"
for /f "usebackq delims= tokens=*" %%A in (`git describe "--abbrev=0" --tags`) do set "VER_TAG=%%A"

> "%_TMP%" (
echo #define AMATSUKAZE_VERSION "%VER_FULL%"
echo #define AMATSUKAZE_PRODUCTVERSION %VER_TAG:.=,%
)

if not exist "%_DST%" (
move /y "%_TMP%" "%_DST%" >nul
endlocal
exit /b 0
)

fc /b "%_TMP%" "%_DST%" >nul
if errorlevel 1 (
move /y "%_TMP%" "%_DST%" >nul
) else (
del "%_TMP%" >nul 2>&1
)

endlocal
