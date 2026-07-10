@echo off
setlocal

set "AOBUS_ROOT=%~dp0"
if "%AOBUS_ROOT:~-1%"=="\" set "AOBUS_ROOT=%AOBUS_ROOT:~0,-1%"
if not defined AOBUS_STATE_ROOT set "AOBUS_STATE_ROOT=%LOCALAPPDATA%\Aobus"

set "CMD=%~1"
if "%CMD%"=="" (
  echo Usage: %~nx0 ^<command^> [args...]
  echo Example: %~nx0 codex
  echo Example: %~nx0 claude
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe was not found at:
  echo   "%VSWHERE%"
  exit /b 1
)

set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "%VSROOT%"=="" (
  echo ERROR: vswhere.exe found no Visual Studio installation with the C++ x64 toolset.
  exit /b 1
)

set "VSDEVCMD=%VSROOT%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo ERROR: VsDevCmd.bat was not found at:
  echo   "%VSDEVCMD%"
  exit /b 1
)

set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo ERROR: vcpkg.exe was not found at:
  echo   "%VCPKG_ROOT%\vcpkg.exe"
  exit /b 1
)

call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 (
  echo ERROR: Failed to initialize the Visual Studio Build Tools environment.
  exit /b %ERRORLEVEL%
)

cd /d "%AOBUS_ROOT%"
if errorlevel 1 (
  echo ERROR: Failed to change directory to:
  echo   "%AOBUS_ROOT%"
  exit /b %ERRORLEVEL%
)

where %CMD% >nul 2>nul
if errorlevel 1 (
  echo ERROR: "%CMD%" was not found on PATH after initializing the build environment.
  echo Install it or add it to PATH, then run this script again.
  exit /b 1
)

set "ARGS="
:parseloop
shift
if "%~1"=="" goto argsdone
set "ARGS=%ARGS% %1"
goto parseloop
:argsdone

echo Starting "%CMD%" in the Visual Studio Build Tools environment...
echo AOBUS_ROOT=%AOBUS_ROOT%
echo AOBUS_STATE_ROOT=%AOBUS_STATE_ROOT%
echo VCPKG_ROOT=%VCPKG_ROOT%

call %CMD%%ARGS%
set "CMD_EXIT=%ERRORLEVEL%"
exit /b %CMD_EXIT%
