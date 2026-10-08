@echo off
rem Aobus WinUI prerequisites launcher (double-click). Runs Install-Prerequisites.ps1
rem in normal current-user install mode. No hardcoded user, password, elevation
rem wrapper or application autolaunch here; the script itself decides what to do.
setlocal
rem A 32-bit shell on 64-bit Windows needs Sysnative to reach 64-bit PowerShell.
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" (
    set "POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
) else (
    set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
)
"%POWERSHELL%" -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Install-Prerequisites.ps1" -Install
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (
    echo Prerequisites are ready. You can start Aobus now.
) else if "%RESULT%"=="1223" (
    echo UAC was declined; the runtime installer was not attempted.
) else if "%RESULT%"=="3010" (
    echo A restart is required to finish prerequisite setup. Restart manually;
    echo nothing was restarted automatically.
) else (
    echo Prerequisite setup did not finish. Exit code %RESULT%.
    echo Read the PowerShell error or JSON report above, if present. A blocked script
    echo may not produce a report; see README.md before retrying.
)
echo.
pause
exit /b %RESULT%
