@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0AD9361TX_UI.exe" goto bundled
where py >nul 2>nul
if not errorlevel 1 goto launcher
where python >nul 2>nul
if not errorlevel 1 goto python
echo AD9361TX UI: Python 3 was not found.
echo Use the complete portable AD9361TX_UI folder or install Python 3 with Tcl/Tk.
pause
exit /b 1

:bundled
start "" "%~dp0AD9361TX_UI.exe" %*
exit /b 0

:launcher
py -3 "%~dp0ad9361tx_ui.py" %*
if not errorlevel 1 exit /b 0
echo AD9361TX UI failed with Python launcher. Retrying installed python...

:python
python "%~dp0ad9361tx_ui.py" %*
if not errorlevel 1 exit /b 0
echo AD9361TX UI startup failed. Check ui_startup_error.log in this folder.
pause
exit /b 1
