@echo off
rem Shared Visual Studio discovery for the Aobus Windows entry scripts
rem (ao.bat, start-msbuild-env.bat). On success it exports to the caller:
rem   VSROOT      Visual Studio installation with the C++ x64 toolset
rem   VSDEVCMD    full path to that installation's VsDevCmd.bat
rem   VCPKG_ROOT  defaulted to the bundled vcpkg when not already set
rem Deliberately no setlocal: the results must reach the caller's scope.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Aobus requires Visual Studio Build Tools with the C++ x64 toolset; vswhere.exe was not found at: 1>&2
  echo   "%VSWHERE%" 1>&2
  exit /b 1
)

set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo Aobus could not find a Visual Studio installation with the C++ x64 toolset. 1>&2
  exit /b 1
)

set "VSDEVCMD=%VSROOT%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo VsDevCmd.bat was not found at: 1>&2
  echo   "%VSDEVCMD%" 1>&2
  exit /b 1
)

if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
exit /b 0
