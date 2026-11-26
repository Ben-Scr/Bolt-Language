@echo off
setlocal DisableDelayedExpansion
rem Resolve tools only for native-build commands. Other commands need no C++ installation.
if /I "%~1"=="compile" goto compiler
if /I "%~1"=="build" goto compiler
if /I "%~1"=="run" goto compiler
goto launch
:compiler
where cl.exe >nul 2>nul
if not errorlevel 1 goto launch
set "BOLT_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%BOLT_VSWHERE%" goto launch
set "BOLT_VS="
for /f "usebackq tokens=*" %%I in (`"%BOLT_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BOLT_VS=%%I"
if not defined BOLT_VS goto launch
if exist "%BOLT_VS%\Common7\Tools\VsDevCmd.bat" call "%BOLT_VS%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 >nul
:launch
if not exist "%~dp0bolt.exe" goto legacy
"%~dp0bolt.exe" %*
exit /b %errorlevel%
:legacy
"%~dp0..\libexec\bolt.exe" %*
exit /b %errorlevel%
