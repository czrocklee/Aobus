@echo off
setlocal

set "ROOT=%~dp0"

if not defined AOBUS_STATE_ROOT (
  if not defined LOCALAPPDATA (
    echo Aobus needs LOCALAPPDATA or an explicit AOBUS_STATE_ROOT for host-local tooling. 1>&2
    exit /b 1
  )
  set "AOBUS_STATE_ROOT=%LOCALAPPDATA%\Aobus"
)
set "AOBUS_STATE_ARGUMENT=%AOBUS_STATE_ROOT%"
if "%AOBUS_STATE_ARGUMENT:~-1%"=="\" set "AOBUS_STATE_ARGUMENT=%AOBUS_STATE_ARGUMENT%."
set "PYTHONIOENCODING=utf-8"
set "PYTHONUTF8=1"
set "PYTHONHOME="

set "NEEDS_BUILD_ENV="
if /I "%~1"=="analyze" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="build" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="check" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="format" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="hygiene" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="run" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="test" set "NEEDS_BUILD_ENV=1"
if /I "%~1"=="tidy" set "NEEDS_BUILD_ENV=1"
call :ensure_python_environment
if errorlevel 1 exit /b %ERRORLEVEL%

if not defined NEEDS_BUILD_ENV goto environment_ready
call :ensure_build_environment
if errorlevel 1 exit /b %ERRORLEVEL%

:environment_ready

for %%i in ("%PYTHON%") do set "PATH=%%~dpi;%PATH%"
pushd "%ROOT%"
"%PYTHON%" -m ao %*
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%

:ensure_python_environment
set "BASE_PYTHON="
if defined AOBUS_PYTHON goto explicit_python

set "BASE_RESULT=%TEMP%\aobus-base-python-%RANDOM%-%RANDOM%.txt"
if defined VCPKG_ROOT goto bootstrap_with_vcpkg
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ROOT%script\ao\bootstrap-python.ps1" -StateRoot "%AOBUS_STATE_ARGUMENT%" -ResultFile "%BASE_RESULT%"
set "BOOTSTRAP_STATUS=%ERRORLEVEL%"
goto bootstrap_finished

:bootstrap_with_vcpkg
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ROOT%script\ao\bootstrap-python.ps1" -StateRoot "%AOBUS_STATE_ARGUMENT%" -ResultFile "%BASE_RESULT%" -VcpkgRoot "%VCPKG_ROOT%"
set "BOOTSTRAP_STATUS=%ERRORLEVEL%"

:bootstrap_finished
if not "%BOOTSTRAP_STATUS%"=="0" (
  del /q "%BASE_RESULT%" >nul 2>nul
  exit /b 1
)
if not exist "%BASE_RESULT%" (
  echo Aobus Python bootstrap did not return an interpreter path. 1>&2
  exit /b 1
)
set /p BASE_PYTHON=<"%BASE_RESULT%"
del /q "%BASE_RESULT%" >nul 2>nul
goto base_python_ready

:explicit_python
set "BASE_PYTHON=%AOBUS_PYTHON%"
if not exist "%AOBUS_PYTHON%" (
  echo AOBUS_PYTHON does not point to a Python executable: 1>&2
  echo   "%AOBUS_PYTHON%" 1>&2
  exit /b 1
)
"%AOBUS_PYTHON%" -I -c "import ensurepip, venv" >nul 2>nul
if errorlevel 1 (
  echo AOBUS_PYTHON must provide a regular Python installation with venv and ensurepip. 1>&2
  exit /b 1
)

:base_python_ready

set "PYTHONPATH=%ROOT%script"
set "ENV_RESULT=%TEMP%\aobus-python-env-%RANDOM%-%RANDOM%.txt"
"%BASE_PYTHON%" -m ao.core.pythonenv --project-root "%ROOT%." --state-root "%AOBUS_STATE_ARGUMENT%" --result-file "%ENV_RESULT%"
if errorlevel 1 (
  del /q "%ENV_RESULT%" >nul 2>nul
  exit /b 1
)
if not exist "%ENV_RESULT%" (
  echo Aobus Python tooling bootstrap did not return an interpreter path. 1>&2
  exit /b 1
)
set /p PYTHON=<"%ENV_RESULT%"
del /q "%ENV_RESULT%" >nul 2>nul
if not exist "%PYTHON%" (
  echo Aobus Python tooling environment is incomplete: 1>&2
  echo   "%PYTHON%" 1>&2
  exit /b 1
)
exit /b 0

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
