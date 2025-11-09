@echo off
for /f "delims=" %%A in ('git describe --tags') do set VER=%%A
REM バージョン番号を正しい形式に変換（例: 0.9.8.7-28-g7f676f2 -> 0.9.8.7）
for /f "tokens=1 delims=-" %%B in ("%VER%") do set SHORTVER=%%B

REM テンプレートからAssemblyInfo.csを生成（ビルドの並列実行時のロックに備えてリトライ）
set TEMPLATE=Properties\AssemblyInfo.tt
set OUTPUT=Properties\AssemblyInfo.cs
for /f "usebackq delims=" %%T in (`powershell -NoProfile -NonInteractive -Command "[IO.Path]::GetTempFileName()"`) do set TEMPFILE=%%T
set MAXATTEMPTS=10
set DELAYMS=200
set /a ATTEMPT=1

REM まずは一時ファイルに生成（再ビルド抑制のため差分がない場合は更新しない）
:RETRY_GEN
powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; try { (Get-Content '%TEMPLATE%') -replace '@VERSION@', '%VER%' -replace '@SHORTVERSION@', '%SHORTVER%' | Set-Content '%TEMPFILE%' -Encoding UTF8 -Force; exit 0 } catch { exit 1 }" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
  set /a ATTEMPT+=1
  if %ATTEMPT% GEQ %MAXATTEMPTS% exit /b %ERRORLEVEL%
  ping -n 1 -w %DELAYMS% 127.0.0.1 >nul
  goto :RETRY_GEN
)

if exist "%OUTPUT%" (
  fc /b "%TEMPFILE%" "%OUTPUT%" >nul
  if %ERRORLEVEL% EQU 0 (
    del "%TEMPFILE%" >nul 2>&1
    exit /b 0
  )
)

REM 差分がある場合のみ上書き（ビルド並列時のロックを想定してリトライ）
:RETRY_MOVE
move /y "%TEMPFILE%" "%OUTPUT%" >nul
if %ERRORLEVEL% EQU 0 goto :EOF

set /a ATTEMPT+=1
if %ATTEMPT% GEQ %MAXATTEMPTS% exit /b %ERRORLEVEL%

REM 200ミリ秒待機して再試行
ping -n 1 -w %DELAYMS% 127.0.0.1 >nul
goto :RETRY_MOVE
