@echo off
for /f "usebackq delims=" %%A in (`git describe --tags`) do set VER=%%A
echo #define AMATSUKAZE_VERSION "%VER%">Version.h

for /f "usebackq delims= tokens=*" %%A in (`git describe "--abbrev=0" --tags`) do set VER=%%A
echo #define AMATSUKAZE_PRODUCTVERSION %VER:.=,% >>Version.h
