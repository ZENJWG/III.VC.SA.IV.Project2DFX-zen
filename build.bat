@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Project2DFX Zeneric Edition - build from Git checkout OR downloaded ZIP.
rem Uses Windows PowerShell included with Windows 10/11.
set "ROOT=%~dp0"
if not exist "%ROOT%tools\build.ps1" (
    echo ERROR: tools\build.ps1 is missing next to build.bat.
    echo Extract the entire project ZIP, not just build.bat.
    exit /b 1
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\build.ps1" %*
exit /b %ERRORLEVEL%
