@echo off
for /f "delims=" %%A in ('git describe --tags') do set VER=%%A
REM バージョン番号を正しい形式に変換（例: 0.9.8.7-28-g7f676f2 -> 0.9.8.7）
for /f "tokens=1 delims=-" %%B in ("%VER%") do set SHORTVER=%%B

REM テンプレートからAssemblyInfo.csを生成（ビルドの並列実行時のロックに備えてリトライ）
set TEMPLATE=Properties\AssemblyInfo.tt
set OUTPUT=Properties\AssemblyInfo.cs
set MAXATTEMPTS=10
set DELAYMS=200
set /a ATTEMPT=1

:RETRY_WRITE
powershell -NoProfile -Command "(Get-Content '%TEMPLATE%') -replace '@VERSION@', '%VER%' -replace '@SHORTVERSION@', '%SHORTVER%' | Set-Content '%OUTPUT%' -Encoding UTF8 -Force"
if %ERRORLEVEL% EQU 0 goto :EOF

if %ATTEMPT% GEQ %MAXATTEMPTS% exit /b %ERRORLEVEL%

REM 200ミリ秒待機して再試行
ping -n 1 -w %DELAYMS% 127.0.0.1 >nul

set /a ATTEMPT+=1
goto :RETRY_WRITE
