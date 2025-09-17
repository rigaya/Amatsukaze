@echo off
setlocal
set CMD=%~n0
if /I "%CMD%"=="ScriptCommand" (
  set SUB=%1
  if not defined SUB (
    echo 使用方法: %~n0 ^<AddTag^|SetOutDir^|SetPriority^|GetOutFiles^|CancelItem^> [args...]
    exit /b 1
  )
  shift
) else (
  set SUB=%CMD%
)
"%~dp0..\ScriptCommand.exe" %SUB% %*
endlocal


