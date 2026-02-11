@echo off

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

if not exist ".\publish\win-x64" mkdir ".\publish\win-x64"
del .\publish\win-x64\*.dll >nul 2>&1
del .\publish\win-x64\*.dll.config >nul 2>&1
del .\publish\win-x64\*.exe >nul 2>&1
del .\publish\win-x64\*.pdb >nul 2>&1

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
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%"
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
if not exist "%VCVARS%" (
  echo vcvars64.bat not found: %VCVARS%
  exit /b 1
)
call "%VCVARS%" x64
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building solution with MSBuild (Release + Release2)...
msbuild Amatsukaze.sln /restore /p:Configuration=Release /p:Platform=x64 /m
if errorlevel 1 exit /b %ERRORLEVEL%
msbuild Amatsukaze.sln /p:Configuration=Release2 /p:Platform=x64 /m
if errorlevel 1 exit /b %ERRORLEVEL%

echo Publishing .NET projects in parallel via Publish.proj...
msbuild Publish.proj /m
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

echo Done. Merged outputs are in %MERGED_DIR% (WebUI is served on REST port+1).
exit /b 0
