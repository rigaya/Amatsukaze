@echo off
for /f "delims=" %%A in ('git describe --tags') do set VER=%%A
REM バージョン番号を正しい形式に変換（例: 0.9.8.7-28-g7f676f2 -> 0.9.8.7）
for /f "tokens=1 delims=-" %%B in ("%VER%") do set SHORTVER=%%B

REM テンプレートからAssemblyInfo.csを生成
powershell -Command "(Get-Content Properties\AssemblyInfo.tt) -replace '@VERSION@', '%VER%' -replace '@SHORTVERSION@', '%SHORTVER%' | Set-Content Properties\AssemblyInfo.cs"
