@echo off
setlocal DisableDelayedExpansion
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
set "PYTHONPATH=%ROOT%script"
if not defined AOBUS_PYTHON goto prepare_working_directory
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

:prepare_working_directory
rem Retain pushd's UNC mapping until the foreground portal has exited. Restore
rem the caller cwd before selection/parsing; only dispatch uses the mapped root.
set "_AOBUS_UNC_MAPPING="
set "_AOBUS_PORTAL_WORKING_DIRECTORY=%ROOT%"
if not "%ROOT:~0,2%"=="\\" goto launch_portal
set "_AOBUS_CALLER_DIRECTORY=%CD%"
pushd "%ROOT%"
if errorlevel 1 exit /b 1
set "_AOBUS_UNC_MAPPING=1"
set "_AOBUS_PORTAL_WORKING_DIRECTORY=%CD%"
cd /d "%_AOBUS_CALLER_DIRECTORY%"
if errorlevel 1 (
  popd
  exit /b 1
)
set "_AOBUS_CALLER_DIRECTORY="

:launch_portal
rem Never CALL the user argument tail: CMD must expand it only once.
if defined AOBUS_PYTHON goto launch_explicit
rem Delayed expansion is used only to snapshot arbitrary environment values;
rem it is disabled again before the original argument tail is referenced.
setlocal EnableDelayedExpansion
set "_AOBUS_ORIGINAL_PS_MODULE_PATH=!PSModulePath!"
set "_AOBUS_ORIGINAL_PS_EXECUTION_POLICY=!PSExecutionPolicyPreference!"
setlocal DisableDelayedExpansion
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ROOT%script\ao\bootstrap-python.ps1" -- %*
set "STATUS=%ERRORLEVEL%"
goto finished

:launch_explicit
set "_AOBUS_ORIGINAL_PS_MODULE_PATH="
set "_AOBUS_ORIGINAL_PS_EXECUTION_POLICY="
"%AOBUS_PYTHON%" -m ao.core.pythonenv --project-root "%ROOT%." --state-root "%AOBUS_STATE_ARGUMENT%" --launch-windows-portal -- %*
set "STATUS=%ERRORLEVEL%"

:finished
if defined _AOBUS_UNC_MAPPING popd
exit /b %STATUS%
