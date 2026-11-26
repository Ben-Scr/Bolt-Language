@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%P in (powershell.exe pwsh.exe) do if not "%%~$PATH:P"=="" (
    set "BOLT_POWERSHELL=%%~$PATH:P"
    goto :install
)
echo Install Windows PowerShell 5.1 or PowerShell 7 and run install.bat again. 1>&2
exit /b 1
:install
"%BOLT_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
exit /b %ERRORLEVEL%
