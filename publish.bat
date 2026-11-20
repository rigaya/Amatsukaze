@echo off
setlocal

cd /d "%~dp0"
if errorlevel 1 (
  echo Failed to change to the publish script directory.
  exit /b 1
)

where dotnet >nul 2>&1
if errorlevel 1 (
  echo dotnet command not found. Install .NET 10 SDK.
  exit /b 1
)
dotnet --list-sdks | findstr /R "^10\." >nul
if errorlevel 1 (
  echo .NET 10 SDK not found. Check dotnet --list-sdks.
  exit /b 1
)

set "VSWHERE="
for /f "usebackq delims=" %%I in (`where vswhere.exe 2^>nul`) do (
  set "VSWHERE=%%~fI"
  goto :vswhere_found
)
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if "%VSWHERE%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if "%VSWHERE%"=="" if exist "%SystemDrive%\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%SystemDrive%\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
:vswhere_found
if "%VSWHERE%"=="" (
  echo vswhere.exe not found in PATH or standard install directories.
  exit /b 1
)

set "VSINSTALL="
set "VSTMP=%TEMP%\amatsukaze_vsinstall.txt"
"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%"
if errorlevel 1 (
  echo vswhere.exe failed.
  if exist "%VSTMP%" del /q "%VSTMP%" >nul 2>&1
  exit /b 1
)
if exist "%VSTMP%" set /p VSINSTALL=<"%VSTMP%"
if exist "%VSTMP%" del /q "%VSTMP%" >nul 2>&1
if "%VSINSTALL%"=="" (
  echo Visual Studio with VC++ tools not found.
  exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%VCVARS%" (
  echo vcvars64.bat not found: %VCVARS%
  exit /b 1
)
if not exist "%MSBUILD%" (
  echo MSBuild.exe not found: %MSBUILD%
  exit /b 1
)

echo Using Visual Studio: %VSINSTALL%
echo Using MSBuild: %MSBUILD%
call "%VCVARS%" x64
if errorlevel 1 exit /b %ERRORLEVEL%

if exist ".\publish\win-x64" rmdir /s /q ".\publish\win-x64"
if exist ".\publish\win-x64" (
  echo Failed to clean the publish output directory.
  exit /b 1
)
mkdir ".\publish\win-x64"
if errorlevel 1 (
  echo Failed to create the publish output directory.
  exit /b 1
)

echo Building solution with MSBuild (Release + Release2)...
"%MSBUILD%" Amatsukaze.sln /restore /p:Configuration=Release /p:Platform=x64 /m
if errorlevel 1 exit /b %ERRORLEVEL%
"%MSBUILD%" Amatsukaze.sln /p:Configuration=Release2 /p:Platform=x64 /m
if errorlevel 1 exit /b %ERRORLEVEL%

echo Publishing .NET projects in parallel via Publish.proj...
"%MSBUILD%" Publish.proj /m
if errorlevel 1 exit /b %ERRORLEVEL%

echo Collecting published artifacts into a single folder...
set MERGED_DIR=.\publish\win-x64

set SRC_BASE=.\publish\win-x64

rem robocopy returns codes <8 for success; treat >=8 as failure
if exist "%SRC_BASE%\AmatsukazeAddTask" robocopy "%SRC_BASE%\AmatsukazeAddTask" "%MERGED_DIR%" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%
if exist "%SRC_BASE%\AmatsukazeServerCLI" robocopy "%SRC_BASE%\AmatsukazeServerCLI" "%MERGED_DIR%" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%
if exist "%SRC_BASE%\AmatsukazeGUI" robocopy "%SRC_BASE%\AmatsukazeGUI" "%MERGED_DIR%" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%
if exist "%SRC_BASE%\ScriptCommand" robocopy "%SRC_BASE%\ScriptCommand" "%MERGED_DIR%" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%
if exist "%SRC_BASE%\AmatsukazeServer\wwwroot" robocopy "%SRC_BASE%\AmatsukazeServer\wwwroot" "%MERGED_DIR%\wwwroot" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%

rem ニコニコ実況コメント取得・ASS変換ツール nicojk_ass(別リポジトリ。danmaku2assを同梱している)
rem リリースの単一のexeのzipをダウンロードして nicojk_ass.exe に置く(ライセンスなどは license\nicojk_ass\ に置く)。版とリポジトリは nicojk_ass_version.txt に書く。
rem NICOJK_ASS_ZIP にローカルのzipを指定すると、ダウンロードの代わりに使う。
rem 旧版の nicojk_ass.py と danmaku2ass.py は使わなくなったので取り除く
if exist "%MERGED_DIR%\nicojk_ass.py" del /f /q "%MERGED_DIR%\nicojk_ass.py"
if exist "%MERGED_DIR%\danmaku2ass.py" del /f /q "%MERGED_DIR%\danmaku2ass.py"
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("nicojk_ass_version.txt") do set "%%A=%%B"
if not defined NICOJK_ASS_VERSION (
  echo nicojk_ass_version.txt から版を読めませんでした。
  exit /b 1
)
set "NICOJK_ASS_TMP=.\publish\nicojk_ass_tmp"
if exist "%NICOJK_ASS_TMP%" rmdir /s /q "%NICOJK_ASS_TMP%"
mkdir "%NICOJK_ASS_TMP%"
if errorlevel 1 exit /b 1
set "NICOJK_ASS_ZIP_PATH=%NICOJK_ASS_ZIP%"
if defined NICOJK_ASS_ZIP_PATH goto nicojk_ass_extract
set "NICOJK_ASS_ZIP_PATH=%NICOJK_ASS_TMP%\nicojk_ass.zip"
set "NICOJK_ASS_URL=%NICOJK_ASS_REPO%/releases/download/%NICOJK_ASS_VERSION%/nicojk_ass-%NICOJK_ASS_VERSION%-windows-x64.zip"
echo nicojk_ass %NICOJK_ASS_VERSION% をダウンロードします: %NICOJK_ASS_URL%
curl.exe -fL --retry 3 --retry-delay 10 -o "%NICOJK_ASS_ZIP_PATH%" "%NICOJK_ASS_URL%"
if errorlevel 1 (
  echo nicojk_ass のダウンロードに失敗しました。
  exit /b 1
)
:nicojk_ass_extract
powershell -NoProfile -Command "Expand-Archive -LiteralPath $env:NICOJK_ASS_ZIP_PATH -DestinationPath $env:NICOJK_ASS_TMP\x -Force"
if errorlevel 1 (
  echo nicojk_ass のzipの展開に失敗しました: %NICOJK_ASS_ZIP_PATH%
  exit /b 1
)
set "NICOJK_ASS_ACTUAL="
pushd "%NICOJK_ASS_TMP%\x"
if errorlevel 1 exit /b 1
for /f "usebackq delims=" %%V in (`nicojk_ass.exe --version`) do set "NICOJK_ASS_ACTUAL=%%V"
popd
if not "%NICOJK_ASS_ACTUAL%"=="nicojk_ass %NICOJK_ASS_VERSION%" (
  echo nicojk_ass の版が一致しません: 期待 "nicojk_ass %NICOJK_ASS_VERSION%"、実際 "%NICOJK_ASS_ACTUAL%"
  exit /b 1
)
rem 以前のフォルダの形(nicojk_ass\)が残っていると、exeを置けないので取り除く
if exist "%MERGED_DIR%\nicojk_ass\" rmdir /s /q "%MERGED_DIR%\nicojk_ass"
copy /y "%NICOJK_ASS_TMP%\x\nicojk_ass.exe" "%MERGED_DIR%\nicojk_ass.exe" >nul
if errorlevel 1 exit /b 1
if exist "%MERGED_DIR%\license\nicojk_ass" rmdir /s /q "%MERGED_DIR%\license\nicojk_ass"
robocopy "%NICOJK_ASS_TMP%\x\danmaku2ass-license" "%MERGED_DIR%\license\nicojk_ass\danmaku2ass-license" /E /NFL /NDL /NJH /NJS
if errorlevel 8 exit /b %ERRORLEVEL%
copy /y "%NICOJK_ASS_TMP%\x\LICENSE" "%MERGED_DIR%\license\nicojk_ass\" >nul
if errorlevel 1 exit /b 1
copy /y "%NICOJK_ASS_TMP%\x\README.md" "%MERGED_DIR%\license\nicojk_ass\" >nul
if errorlevel 1 exit /b 1
rmdir /s /q "%NICOJK_ASS_TMP%"
rem NicoConvASSの実況取得に使う局対応表(nicojk_assは自分の表を持つ)
for %%F in (ch_sid.txt) do (
  copy /y ".\defaults\exe_files\%%F" "%MERGED_DIR%\%%F" >nul
  if errorlevel 1 (
    echo %%F の配置に失敗しました。
    exit /b 1
  )
)
echo Done. Merged outputs are in %MERGED_DIR% (WebUI is served on REST port+1).
exit /b 0
