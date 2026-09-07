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
x64\Release\AmatsukazeNativeTests.exe
exit /b %ERRORLEVEL%
