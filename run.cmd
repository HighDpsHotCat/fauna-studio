@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    py -3.10 -m venv .venv
    if errorlevel 1 goto :failed
)
".venv\Scripts\python.exe" bootstrap.py
if errorlevel 1 goto :failed
start "" ".venv\Scripts\pythonw.exe" "%~dp0app.py"
exit /b 0
:failed
echo Setup failed. Install 64-bit Python 3.10 and check your internet connection.
pause
exit /b 1
