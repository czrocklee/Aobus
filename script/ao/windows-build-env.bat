@echo off
rem No application arguments enter this CMD process. Only VS setup belongs here.
if defined VCPKG_ROOT goto vcpkg_ready
call "%~dp0windows-vsenv.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

:vcpkg_ready
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo Aobus could not find the vcpkg CMake toolchain under VCPKG_ROOT: 1>&2
  echo   "%VCPKG_ROOT%" 1>&2
  exit /b 1
)
where cl.exe >nul 2>nul
if not errorlevel 1 goto environment_ready
call "%~dp0windows-vsenv.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
  echo Aobus failed to initialize the Visual Studio x64 build environment. 1>&2
  exit /b %ERRORLEVEL%
)

:environment_ready
rem Escaped ASCII JSON is independent of the CMD console codepage. Do not log it.
"%_AOBUS_ENV_PYTHON%" -I -c "import json, os; print(json.dumps(dict(os.environ)))"
exit /b %ERRORLEVEL%
