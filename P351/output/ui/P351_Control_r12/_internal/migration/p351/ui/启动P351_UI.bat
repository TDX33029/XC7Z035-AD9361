@echo off
cd /d "%~dp0"
if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" (
  "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" "%~dp0run_p351_ui.py"
) else (
  python "%~dp0run_p351_ui.py"
)
if errorlevel 1 pause
