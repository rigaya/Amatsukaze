@echo off

for /f "delims=" %%i in ("%IN_PATH%") do set IN_DIR=%%~dpi

move "%IN_PATH%" "%IN_DIR%.."

move "%IN_PATH%.err" "%IN_DIR%.."
move "%IN_PATH%.program.txt" "%IN_DIR%.."
move "%IN_PATH%.trim.avs" "%IN_DIR%.."