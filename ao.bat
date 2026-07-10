@echo off
setlocal

set "ROOT=%~dp0"

set "NEEDS_BUILD_ENV="
if /I "%~1"=="analyze" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="build" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="check" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="format" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="hygiene" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="run" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="test" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="tidy" set "NEEDS_BUILD_ENV=1"
if not defined NEEDS_BUILD_ENV goto environment_ready
call :ensure_build_environment
if errorlevel 1 exit /b %ERRORLEVEL%

:environment_ready

set "PYTHON=%ROOT%vcpkg_installed\x64-windows\tools\python3\python.exe"

if not exist "%PYTHON%" (
  where python.exe >nul 2>nul
  if errorlevel 1 (
    echo Aobus Python was not found. Install Python 3 and add python.exe to PATH. 1>&2
    exit /b 1
  )
  set "PYTHON=python.exe"
)

set "PYTHONPATH=%ROOT%script;%PYTHONPATH%"
pushd "%ROOT%"
"%PYTHON%" -m ao %*
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%

:ensure_build_environment
if defined VCPKG_ROOT goto vcpkg_ready
call :find_visual_studio
if errorlevel 1 exit /b %ERRORLEVEL%

:vcpkg_ready
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo Aobus could not find the vcpkg CMake toolchain under VCPKG_ROOT: 1>&2
  echo   "%VCPKG_ROOT%" 1>&2
  exit /b 1
)

where cl.exe >nul 2>nul
if not errorlevel 1 exit /b 0

call :find_visual_studio
if errorlevel 1 exit /b %ERRORLEVEL%
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
  echo Aobus failed to initialize the Visual Studio x64 build environment. 1>&2
  exit /b %ERRORLEVEL%
)
exit /b 0

:find_visual_studio
if defined VSROOT exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Aobus requires Visual Studio Build Tools with the C++ x64 toolset. 1>&2
  exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VSROOT=%%i"
  if not defined VCPKG_ROOT set "VCPKG_ROOT=%%i\VC\vcpkg"
)
if not defined VSROOT (
  echo Aobus could not find a Visual Studio installation with the C++ x64 toolset. 1>&2
  exit /b 1
)
exit /b 0
