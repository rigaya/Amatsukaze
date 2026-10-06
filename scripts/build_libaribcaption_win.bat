@echo off
setlocal EnableExtensions

rem libaribcaptionを取得・パッチ適用し、Amatsukaze.dllへ静的リンクするライブラリをビルドする。
rem Amatsukaze.vcxprojのビルド前イベントから呼ばれる。
rem 引数: Platform(x64/Win32) Configuration PlatformToolset VsInstallRoot VisualStudioVersion
rem バージョンとアーカイブはLinux側(scripts/build_caption_deps.sh)と揃える。
rem 成果物は build\libaribcaption\<Platform>\<Configuration>\install に出力する。
rem 入力(バージョン・パッチ・ツールセット等)が前回と同じなら何もしない。

set "ARIBCC_VERSION=1.1.2"
set "ARIBCC_SHA256=649b50bde99272b97c66af2a8400163e2f84eae072d252daa26baaaf0866a1c2"
set "ARIBCC_URL=https://codeload.github.com/xqq/libaribcaption/tar.gz/refs/tags/v%ARIBCC_VERSION%"

set "AMT_PLATFORM=%~1"
set "AMT_CONFIG=%~2"
set "AMT_TOOLSET=%~3"
set "AMT_VSROOT=%~4"
set "AMT_VSVER=%~5"
if "%AMT_VSVER%"=="" (
  echo [libaribcaption] usage: %~nx0 Platform Configuration PlatformToolset VsInstallRoot VisualStudioVersion
  exit /b 1
)
if "%AMT_VSROOT:~-1%"=="\" set "AMT_VSROOT=%AMT_VSROOT:~0,-1%"

for %%I in ("%~dp0..") do set "AMT_ROOT=%%~fI"
set "PATCH_DIR=%~dp0patches\libaribcaption-%ARIBCC_VERSION%"
set "ARIBCC_BASE=%AMT_ROOT%\build\libaribcaption"
set "ARIBCC_ARCHIVE=%ARIBCC_BASE%\libaribcaption-%ARIBCC_VERSION%.tar.gz"
set "WORK_DIR=%ARIBCC_BASE%\%AMT_PLATFORM%\%AMT_CONFIG%"
rem git applyの--directoryはリポジトリ(またはカレント)のルートからの相対パスで指定する
set "SRC_REL=build/libaribcaption/%AMT_PLATFORM%/%AMT_CONFIG%/src/libaribcaption-%ARIBCC_VERSION%"
set "SRC_DIR=%WORK_DIR%\src\libaribcaption-%ARIBCC_VERSION%"
set "CMAKE_BUILD_DIR=%WORK_DIR%\cmake_build"
set "INSTALL_DIR=%WORK_DIR%\install"
set "ARIBCC_LIB=%INSTALL_DIR%\lib\aribcaption.lib"
set "STAMP=%ARIBCC_BASE%\%AMT_PLATFORM%_%AMT_CONFIG%.stamp"
set "STAMP_NEW=%STAMP%.new"

rem Debug系構成は/MTd、それ以外は/MTでAmatsukaze.vcxprojとCRTを揃える
if /i "%AMT_CONFIG:~0,5%"=="Debug" (
  set "CMAKE_CONFIG=Debug"
  set "CMAKE_RUNTIME=MultiThreadedDebug"
) else (
  set "CMAKE_CONFIG=Release"
  set "CMAKE_RUNTIME=MultiThreaded"
)

rem 呼び出し元のMSBuildと同じVisual Studioのジェネレータを使う
set "CMAKE_GENERATOR="
if "%AMT_VSVER%"=="17.0" set "CMAKE_GENERATOR=Visual Studio 17 2022"
if "%AMT_VSVER%"=="18.0" set "CMAKE_GENERATOR=Visual Studio 18 2026"
if "%CMAKE_GENERATOR%"=="" (
  echo [libaribcaption] Unsupported VisualStudioVersion: %AMT_VSVER%
  exit /b 1
)

rem Visual Studio同梱のCMake("C++ CMake tools for Windows")を優先し、なければPATHから探す
set "CMAKE_EXE=%AMT_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE_EXE%" (
  set "CMAKE_EXE="
  for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%~fI"
)
if not defined CMAKE_EXE (
  echo [libaribcaption] cmake.exe not found. Install "C++ CMake tools for Windows" in Visual Studio Installer.
  exit /b 1
)

if not exist "%ARIBCC_BASE%" mkdir "%ARIBCC_BASE%"
if errorlevel 1 exit /b 1

rem 今回の入力をスタンプとして書き出し、前回ビルド時のスタンプと比較する
> "%STAMP_NEW%" (
  echo version=%ARIBCC_VERSION%
  echo sha256=%ARIBCC_SHA256%
  echo toolset=%AMT_TOOLSET%
  echo generator=%CMAKE_GENERATOR%
  echo vsroot=%AMT_VSROOT%
  echo config=%CMAKE_CONFIG%
  echo runtime=%CMAKE_RUNTIME%
)
if errorlevel 1 exit /b 1
for %%P in ("%PATCH_DIR%\*.patch") do call :append_patch_stamp "%%~fP"
if exist "%STAMP%" if exist "%ARIBCC_LIB%" (
  fc /b "%STAMP%" "%STAMP_NEW%" >nul 2>&1
  if not errorlevel 1 (
    del /q "%STAMP_NEW%" >nul 2>&1
    echo [libaribcaption] %AMT_PLATFORM% %AMT_CONFIG%: up to date.
    exit /b 0
  )
)

echo [libaribcaption] Building libaribcaption %ARIBCC_VERSION% for %AMT_PLATFORM% %AMT_CONFIG% ^(%AMT_TOOLSET%, %CMAKE_RUNTIME%^)...
if exist "%STAMP%" del /q "%STAMP%"

rem アーカイブは構成間で共有する。途中で中断しても壊れたファイルが残らないよう一時ファイル経由で配置する
if not exist "%ARIBCC_ARCHIVE%" (
  echo [libaribcaption] Downloading %ARIBCC_URL%
  curl.exe -fsSL --retry 3 --retry-delay 5 -o "%ARIBCC_ARCHIVE%.%RANDOM%.tmp" "%ARIBCC_URL%"
  if errorlevel 1 (
    echo [libaribcaption] Download failed: %ARIBCC_URL%
    del /q "%ARIBCC_ARCHIVE%.*.tmp" >nul 2>&1
    exit /b 1
  )
  for %%T in ("%ARIBCC_ARCHIVE%.*.tmp") do move /y "%%~fT" "%ARIBCC_ARCHIVE%" >nul
)
call :sha256 "%ARIBCC_ARCHIVE%" ARCHIVE_HASH
if /i not "%ARCHIVE_HASH%"=="%ARIBCC_SHA256%" (
  echo [libaribcaption] SHA256 mismatch: %ARIBCC_ARCHIVE%
  echo [libaribcaption]   expected: %ARIBCC_SHA256%
  echo [libaribcaption]   actual  : %ARCHIVE_HASH%
  del /q "%ARIBCC_ARCHIVE%" >nul 2>&1
  exit /b 1
)

rem パッチ適用済みのソースを再利用しないよう、毎回展開し直す
if exist "%WORK_DIR%" rmdir /s /q "%WORK_DIR%"
mkdir "%WORK_DIR%\src"
if errorlevel 1 exit /b 1
tar -xf "%ARIBCC_ARCHIVE%" -C "%WORK_DIR%\src"
if errorlevel 1 (
  echo [libaribcaption] Failed to extract %ARIBCC_ARCHIVE%
  exit /b 1
)

for %%P in ("%PATCH_DIR%\*.patch") do (
  echo [libaribcaption] Applying %%~nxP
  git -C "%AMT_ROOT%" apply --directory="%SRC_REL%" "%%~fP"
  if errorlevel 1 (
    echo [libaribcaption] Failed to apply %%~nxP
    exit /b 1
  )
)

"%CMAKE_EXE%" -S "%SRC_DIR%" -B "%CMAKE_BUILD_DIR%" -G "%CMAKE_GENERATOR%" -A %AMT_PLATFORM% -T %AMT_TOOLSET% ^
  -DCMAKE_GENERATOR_INSTANCE="%AMT_VSROOT%" -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=%CMAKE_RUNTIME% -DBUILD_SHARED_LIBS=OFF -DARIBCC_SHARED_LIBRARY=OFF ^
  -DARIBCC_BUILD_TESTS=OFF -DARIBCC_USE_DIRECTWRITE=ON -DARIBCC_USE_GDI_FONT=OFF ^
  -DARIBCC_USE_FREETYPE=OFF -DARIBCC_USE_EMBEDDED_FREETYPE=OFF
if errorlevel 1 (
  echo [libaribcaption] CMake configure failed.
  exit /b 1
)
"%CMAKE_EXE%" --build "%CMAKE_BUILD_DIR%" --config %CMAKE_CONFIG% --parallel
if errorlevel 1 (
  echo [libaribcaption] CMake build failed.
  exit /b 1
)
"%CMAKE_EXE%" --install "%CMAKE_BUILD_DIR%" --config %CMAKE_CONFIG%
if errorlevel 1 (
  echo [libaribcaption] CMake install failed.
  exit /b 1
)
if not exist "%ARIBCC_LIB%" (
  echo [libaribcaption] Library not found after install: %ARIBCC_LIB%
  exit /b 1
)

move /y "%STAMP_NEW%" "%STAMP%" >nul
if errorlevel 1 exit /b 1
echo [libaribcaption] Done: %INSTALL_DIR%
exit /b 0

rem パッチのファイル名とSHA256をスタンプへ追記する
:append_patch_stamp
call :sha256 "%~1" PATCH_HASH
>> "%STAMP_NEW%" echo patch=%~nx1 %PATCH_HASH%
exit /b 0

rem ファイルのSHA256を小文字16進(空白なし)で第2引数の変数へ設定する
:sha256
set "%~2="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%~1" SHA256') do if not defined %~2 set "%~2=%%H"
call set "%~2=%%%~2: =%%"
exit /b 0
