@echo off

for /f %%i in ('AmatsukazeCLI -i "%IN_PATH%" -s %SERVICE_ID% --mode probe_subtitles') do if %%i == �������� (AddTag ����)

for /f "tokens=2" %%i in ('AmatsukazeCLI -i "%IN_PATH%" -s %SERVICE_ID% --mode probe_audio') do if %%i == 1 (AddTag ������)
