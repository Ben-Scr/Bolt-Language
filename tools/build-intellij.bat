@echo off
where py >nul 2>nul
if %errorlevel% equ 0 (
  py -3 "%~dp0build-intellij.py" %*
) else (
  python "%~dp0build-intellij.py" %*
)
exit /b %errorlevel%
