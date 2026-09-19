@echo off
setlocal

cd /d "%~dp0\..\.."
if errorlevel 1 exit /b %ERRORLEVEL%

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
  echo vswhere.exe が見つかりません。
  exit /b 1
)

set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if "%VSINSTALL%"=="" (
  echo C++ ツールを含む Visual Studio が見つかりません。
  exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%VCVARS%" (
  echo vcvars64.bat が見つかりません: %VCVARS%
  exit /b 1
)
if not exist "%MSBUILD%" (
  echo MSBuild.exe が見つかりません: %MSBUILD%
  exit /b 1
)

call "%VCVARS%" x64
if errorlevel 1 exit /b %ERRORLEVEL%
"%MSBUILD%" Amatsukaze.sln /t:AmatsukazeNativeTests /p:Configuration=Release /p:Platform=x64 /m:1
if errorlevel 1 exit /b %ERRORLEVEL%

set "TEST_RUNTIME=%CD%\x64\Release"
if not "%AMT_NATIVE_TEST_RUNTIME_DIR%"=="" (
  set "TEST_RUNTIME=%AMT_NATIVE_TEST_RUNTIME_DIR%"
  if not exist "%AMT_NATIVE_TEST_RUNTIME_DIR%" (
    echo テスト用ランタイムディレクトリが見つかりません: %AMT_NATIVE_TEST_RUNTIME_DIR%
    exit /b 1
  )

  rem リリースパッケージと同じ順序でベースパッケージへ今回のビルド成果物を上書きする。
  copy /y x64\Release\*.dll "%AMT_NATIVE_TEST_RUNTIME_DIR%" >nul
  if errorlevel 1 exit /b 1
  copy /y x64\Release\AmatsukazeNativeTests.exe "%AMT_NATIVE_TEST_RUNTIME_DIR%" >nul
  if errorlevel 1 exit /b 1
  if exist lib\x64\*.dll copy /y lib\x64\*.dll "%AMT_NATIVE_TEST_RUNTIME_DIR%" >nul
  if errorlevel 1 exit /b 1

  rem FFmpeg DLLの名前と探索方法はリリースパッケージ作成CIに合わせる。
  for /r ffmpeg_lgpl %%D in (avcodec-61.dll avdevice-61.dll avfilter-10.dll avformat-61.dll avutil-59.dll swresample-5.dll swscale-8.dll) do if exist "%%D" (
    copy /y "%%D" "%AMT_NATIVE_TEST_RUNTIME_DIR%" >nul
    if errorlevel 1 exit /b 1
  )

  rem 現在のAmatsukaze.dllがリンクする旧FFmpeg DLLと、リリースへ同梱する新FFmpeg DLLの両方を確認する。
  for %%F in (AviSynth.dll Caption.dll libfaad2.dll avcodec-58.dll avformat-58.dll avutil-56.dll swscale-5.dll avcodec-61.dll) do (
    if not exist "%AMT_NATIVE_TEST_RUNTIME_DIR%\%%F" (
      echo テスト用ランタイムに%%Fがありません。
      exit /b 1
    )
  )

  dumpbin /dependents "%AMT_NATIVE_TEST_RUNTIME_DIR%\Amatsukaze.dll"
  if errorlevel 1 exit /b 1
)

pushd "%TEST_RUNTIME%"
if errorlevel 1 exit /b %ERRORLEVEL%
AmatsukazeNativeTests.exe
set "TEST_EXIT=%ERRORLEVEL%"
popd
exit /b %TEST_EXIT%
