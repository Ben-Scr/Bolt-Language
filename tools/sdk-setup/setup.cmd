@echo off
setlocal EnableExtensions DisableDelayedExpansion
for %%P in (powershell.exe pwsh.exe) do if not "%%~$PATH:P"=="" (
    set "BOLT_POWERSHELL=%%~$PATH:P"
    goto :install
)
echo Windows PowerShell 5.1 or PowerShell 7 is required. 1>&2
exit /b 1
:install
"%BOLT_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdk-setup.ps1" -Action Install %*
exit /b %ERRORLEVEL%
