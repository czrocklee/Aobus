@echo off
rem No application arguments enter this CMD process. Only VS setup belongs here.
if defined VCPKG_ROOT goto vcpkg_ready
if defined VSDEVCMD goto vcpkg_beside_selection
call "%~dp0windows-vsenv.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
goto vcpkg_ready

:vcpkg_beside_selection
rem An explicit VsDevCmd selection is preserved: default the bundled vcpkg
rem beside that installation instead of discovering a different latest one.
if not exist "%VSDEVCMD%" (
  echo Aobus could not find the explicitly selected Visual Studio VsDevCmd.bat: 1>&2
  echo   "%VSDEVCMD%" 1>&2
  exit /b 1
)
rem <VSROOT>\Common7\Tools\VsDevCmd.bat -> <VSROOT>\VC\vcpkg
for %%i in ("%VSDEVCMD%") do set "VCPKG_ROOT=%%~dpi..\..\VC\vcpkg"
for %%i in ("%VCPKG_ROOT%") do set "VCPKG_ROOT=%%~fi"

:vcpkg_ready
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo Aobus could not find the vcpkg CMake toolchain under VCPKG_ROOT: 1>&2
  echo   "%VCPKG_ROOT%" 1>&2
  exit /b 1
)
rem Reuse the caller's environment only when it is already a fully initialized
rem x64 host/target Visual Studio environment: cl.exe on PATH, both VsDevCmd
rem architecture arguments x64, every required selector, and INCLUDE/LIB.
rem Anything less silently admits an arbitrary or incomplete compiler, so
rem initialize VsDevCmd below instead.
where cl.exe >nul 2>nul
if errorlevel 1 goto vs_setup
if /i not "%VSCMD_ARG_HOST_ARCH%"=="x64" goto vs_setup
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" goto vs_setup
if not defined VSINSTALLDIR goto vs_setup
if not defined VCToolsInstallDir goto vs_setup
if not defined WindowsSdkDir goto vs_setup
if not defined VCToolsVersion goto vs_setup
if not defined WindowsSDKVersion goto vs_setup
if not defined INCLUDE goto vs_setup
if not defined LIB goto vs_setup
goto environment_ready

:vs_setup
if defined VSDEVCMD goto vsdevcmd_ready
call "%~dp0windows-vsenv.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

:vsdevcmd_ready
if not exist "%VSDEVCMD%" (
  echo Aobus could not find the selected Visual Studio VsDevCmd.bat: 1>&2
  echo   "%VSDEVCMD%" 1>&2
  exit /b 1
)
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
  echo Aobus failed to initialize the Visual Studio x64 build environment. 1>&2
  exit /b %ERRORLEVEL%
)

:environment_ready
rem Escaped ASCII JSON is independent of the CMD console codepage. Do not log it.
"%_AOBUS_ENV_PYTHON%" -I -c "import json, os; print(json.dumps(dict(os.environ)))"
exit /b %ERRORLEVEL%
