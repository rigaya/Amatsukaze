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

del .\publish\win-x64\*.dll
del .\publish\win-x64\*.dll.config
del .\publish\win-x64\*.exe
del .\publish\win-x64\*.pdb

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" x64

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
