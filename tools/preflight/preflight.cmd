@echo off
rem Runs preflight.ps1 under Windows PowerShell 5.1 with the arguments given here, then waits for a
rem key so that a double-clicked window stays open. Set KYTY_PREFLIGHT_NO_PAUSE=1 to skip the wait.
setlocal
if not exist "%~dp0preflight.ps1" (
    echo preflight.ps1 is not next to this file. Extract the whole zip first ^(right-click it, Extract All^),
    echo then run preflight.cmd from the extracted preflight folder.
    if not defined KYTY_PREFLIGHT_NO_PAUSE pause
    exit /b 2
)
set "KYTY_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%KYTY_POWERSHELL%" set "KYTY_POWERSHELL=powershell.exe"
"%KYTY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0preflight.ps1" %*
set "KYTY_EXIT=%ERRORLEVEL%"
if not defined KYTY_PREFLIGHT_NO_PAUSE pause
exit /b %KYTY_EXIT%
