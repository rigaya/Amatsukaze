@echo off
for /f "delims=" %%a in ("%OUT_PATH%") do set DST=%%~dpaƒƒCƒ““®‰æˆÈŠO
mkdir "%DST%"
for /f "delims=" %%a in ('GetOutFiles cwdtl') do set FILES=%%a
:loop
for /f "tokens=1* delims=;" %%a in ("%FILES%") do (
   move /Y "%%a" "%DST%"
   set FILES=%%b
   )
if defined FILES goto :loop